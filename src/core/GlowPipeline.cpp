#include "GlowPipeline.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "Blur.h"
#include "Color.h"
#include "ImageF.h"
#include "Resample.h"
#include "Transfer.h"

namespace abglow {
namespace {

constexpr float kOpaque = 0.999f;
constexpr float kTransparent = 1.0e-6f;

struct Threshold {
    float level = 0.0f;
    float knee = 0.0f;
    float saturation_bias = 0.0f;
    bool unmult = false;
};

struct PixelPoint {
    int x = 0;
    int y = 0;
};

inline int CeilDiv(int value, int divisor) {
    return value >= 0 ? (value + divisor - 1) / divisor : -((-value) / divisor);
}

inline int FloorToMultiple(int value, int modulus) {
    const int quotient = value >= 0 ? value / modulus : -((-value + modulus - 1) / modulus);
    return quotient * modulus;
}

inline PixelF LinearizePremultiplied(const PixelF& p, const TransferFunction& transfer) {
    if (transfer.IsIdentity()) return p;
    if (p.a >= kOpaque) return PixelF{p.a, transfer.Decode(p.r), transfer.Decode(p.g), transfer.Decode(p.b)};
    if (p.a <= kTransparent) return PixelF{p.a, 0.0f, 0.0f, 0.0f};
    const float inv = 1.0f / p.a;
    return PixelF{p.a, transfer.Decode(p.r * inv) * p.a, transfer.Decode(p.g * inv) * p.a,
                  transfer.Decode(p.b * inv) * p.a};
}

// A host pixel as premultiplied linear light, whichever convention it is
// stored in. A straight pixel's colour is the pixel's own colour, so it is
// decoded as it stands and then weighted by its coverage. Read as if it were
// premultiplied, a straight anti-aliased edge divided its full colour by its
// coverage a second time: a pixel a tenth covered glowed ten times too bright,
// which put a ragged, flickering fringe of light along every edge and made the
// glow stronger at reduced resolution, where more of a layer is edge.
inline PixelF LinearPremultiplied(const PixelF& p, const TransferFunction& transfer, AlphaMode mode) {
    if (mode == AlphaMode::kPremultiplied) return LinearizePremultiplied(p, transfer);
    if (p.a <= kTransparent) return PixelF{p.a, 0.0f, 0.0f, 0.0f};
    return PixelF{p.a, transfer.Decode(p.r) * p.a, transfer.Decode(p.g) * p.a, transfer.Decode(p.b) * p.a};
}

// A host pixel premultiplied in its own encoding: what it shows over black.
inline PixelF Premultiplied(const PixelF& p, AlphaMode mode) {
    if (mode == AlphaMode::kPremultiplied) return p;
    if (p.a <= kTransparent) return PixelF{p.a, 0.0f, 0.0f, 0.0f};
    return PixelF{p.a, p.r * p.a, p.g * p.a, p.b * p.a};
}

// The composite's premultiplied result in the destination's convention.
inline PixelF ToHostAlpha(const PixelF& p, AlphaMode mode) {
    if (mode == AlphaMode::kPremultiplied) return p;
    if (!(p.a > kTransparent)) return PixelF{0.0f, 0.0f, 0.0f, 0.0f};
    const float inv = 1.0f / p.a;
    return PixelF{p.a, p.r * inv, p.g * inv, p.b * inv};
}

// Soft-knee highlight isolation. The brightest channel drives the threshold so
// saturated colours glow as readily as white, and above the knee the HDR value
// passes through untouched.
//
// The threshold judges the pixel's own brightness, not its brightness times its
// coverage: a half-covered pixel on the edge of a bright glyph is bright, it
// just covers less area, and it should emit half the light rather than be
// rejected for being dim. Testing the premultiplied value instead makes the
// extracted light a non-linear function of coverage, so the same layer emits
// measurably less once After Effects has downsampled it for a reduced render
// resolution - 8% less at Quarter on anti-aliased text.
inline PixelF ExtractHighlight(const PixelF& linear, const Threshold& threshold) {
    // Premultiplied pixels with no alpha emit no light, whatever RGB they carry.
    if (linear.a <= kTransparent && !threshold.unmult) return PixelF{0.0f, 0.0f, 0.0f, 0.0f};
    const float inv_coverage = linear.a >= kOpaque ? 1.0f : 1.0f / std::max(linear.a, kTransparent);
    const float level = std::max(linear.r, std::max(linear.g, linear.b)) * inv_coverage;
    if (level <= 0.0f) return PixelF{0.0f, 0.0f, 0.0f, 0.0f};

    float above = level - threshold.level;
    if (threshold.knee > 0.0f) {
        float soft = std::clamp(above + threshold.knee, 0.0f, 2.0f * threshold.knee);
        soft = soft * soft / (4.0f * threshold.knee);
        above = std::max(soft, above);
    }
    if (above <= 0.0f) return PixelF{0.0f, 0.0f, 0.0f, 0.0f};

    float contribution = above / std::max(level, 1.0e-6f);
    if (threshold.saturation_bias != 0.0f) {
        const float low = std::min(linear.r, std::min(linear.g, linear.b)) * inv_coverage;
        const float saturation = level > 0.0f ? 1.0f - low / level : 0.0f;
        contribution *= std::max(0.0f, 1.0f + threshold.saturation_bias * saturation);
    }
    // Unmult reads coverage from the brightest channel, so black is treated as
    // empty rather than as an opaque black that emits nothing.
    const float coverage = threshold.unmult ? std::clamp(level, 0.0f, 1.0f) : linear.a;
    return PixelF{coverage * contribution, linear.r * contribution * (threshold.unmult ? inv_coverage * coverage : 1.0f),
                  linear.g * contribution * (threshold.unmult ? inv_coverage * coverage : 1.0f),
                  linear.b * contribution * (threshold.unmult ? inv_coverage * coverage : 1.0f)};
}

// Taps of a tent prefilter for one axis. Decimating with a box places each
// sample at its cell's centre, which preserves the light but not its centre of
// mass, so a shape moving across the grid makes the glow lead and lag by up to
// a twelfth of a cell - a sawtooth with the period of the pyramid step, and the
// shimmer that goes with it. A tent spanning two cells reproduces linear
// functions, so first moments survive the decimation. At scale 1 it degenerates
// to (0, 1, 0) and costs nothing.
struct TentTaps {
    int first = 0;
    int count = 0;
    float weight[2 * kMaxBaseScale + 2] = {};
};

TentTaps MakeTentTaps(int scale) {
    TentTaps taps;
    const float span = static_cast<float>(scale);
    const float centre = 0.5f * span;
    taps.first = static_cast<int>(std::ceil(centre - span - 0.5f));
    const int last = static_cast<int>(std::floor(centre + span - 0.5f));
    taps.count = std::min(last - taps.first + 1, static_cast<int>(sizeof(taps.weight) / sizeof(float)));
    float sum = 0.0f;
    for (int i = 0; i < taps.count; ++i) {
        const float d = static_cast<float>(taps.first + i) + 0.5f - centre;
        const float w = 1.0f - std::fabs(d) / span;
        taps.weight[i] = w > 0.0f ? w : 0.0f;
        sum += taps.weight[i];
    }
    // Normalised over the whole support, including taps that fall outside the
    // layer: a cell hanging over the edge must dim, not be renormalised back up.
    if (sum > 0.0f) {
        for (int i = 0; i < taps.count; ++i) taps.weight[i] /= sum;
    }
    return taps;
}

template <PixelDepth kDepth>
void ExtractSourceRow(const HostImage& source, int src_y, const Threshold& threshold,
                      const TransferFunction& transfer, PixelF* out) {
    const void* row = source.ConstRow(src_y);
    for (int x = 0; x < source.width; ++x) {
        out[x] = ExtractHighlight(LinearPremultiplied(ReadRowPixel<kDepth>(row, x), transfer, source.alpha),
                                  threshold);
    }
}

// One level-0 row, horizontally filtered: level-0 column lx gathers source
// pixels lx * scale + block_offset + taps.first ... for taps.count of them.
void FilterRowHorizontal(const PixelF* extracted, int source_width, int scale, int block_offset,
                         const TentTaps& taps, int level0_width, PixelF* out) {
    for (int lx = 0; lx < level0_width; ++lx) {
        const int base = lx * scale + block_offset + taps.first;
        PixelF acc{0.0f, 0.0f, 0.0f, 0.0f};
        const int begin = std::max(0, -base);
        const int end = std::min(taps.count, source_width - base);
        for (int i = begin; i < end; ++i) {
            const PixelF& p = extracted[base + i];
            const float w = taps.weight[i];
            acc.a += p.a * w;
            acc.r += p.r * w;
            acc.g += p.g * w;
            acc.b += p.b * w;
        }
        out[lx] = acc;
    }
}

template <PixelDepth kDepth>
void ExtractRows(const HostImage& source, int offset_x, int offset_y, int scale, const PixelPoint& grid_start,
                 const Threshold& threshold, const TransferFunction& transfer, ImageF& level0, int y_begin,
                 int y_end) {
    // Source x of level-0 column lx is lx * scale + block_offset_x.
    const int block_offset_x = grid_start.x + offset_x;
    const int block_offset_y = grid_start.y + offset_y;
    const TentTaps taps = MakeTentTaps(scale);

    std::vector<PixelF> extracted(static_cast<std::size_t>(source.width));
    // Ring of horizontally filtered source rows, so each is built once even
    // though consecutive level-0 rows share most of them.
    const int slots = taps.count;
    std::vector<PixelF> ring(static_cast<std::size_t>(slots) * static_cast<std::size_t>(level0.width));
    std::vector<int> ring_row(static_cast<std::size_t>(slots), -1);

    auto row_for = [&](int src_y) -> const PixelF* {
        const int slot = ((src_y % slots) + slots) % slots;
        PixelF* out = ring.data() + static_cast<std::size_t>(slot) * static_cast<std::size_t>(level0.width);
        if (ring_row[static_cast<std::size_t>(slot)] != src_y) {
            ExtractSourceRow<kDepth>(source, src_y, threshold, transfer, extracted.data());
            FilterRowHorizontal(extracted.data(), source.width, scale, block_offset_x, taps, level0.width, out);
            ring_row[static_cast<std::size_t>(slot)] = src_y;
        }
        return out;
    };

    for (int ly = y_begin; ly < y_end; ++ly) {
        PixelF* out = level0.Row(ly);
        for (int lx = 0; lx < level0.width; ++lx) out[lx] = PixelF{0.0f, 0.0f, 0.0f, 0.0f};

        const int base = ly * scale + block_offset_y + taps.first;
        const int begin = std::max(0, -base);
        const int end = std::min(taps.count, source.height - base);
        for (int j = begin; j < end; ++j) {
            const PixelF* row = row_for(base + j);
            const float w = taps.weight[j];
            for (int lx = 0; lx < level0.width; ++lx) {
                out[lx].a += row[lx].a * w;
                out[lx].r += row[lx].r * w;
                out[lx].g += row[lx].g * w;
                out[lx].b += row[lx].b * w;
            }
        }
    }
}

void ExtractHighlights(const HostImage& source, int offset_x, int offset_y, int scale, const PixelPoint& grid_start,
                       const Threshold& threshold, const TransferFunction& transfer, ImageF& level0,
                       TaskRunner& runner) {
    ParallelRows(runner, level0.height, [&](int begin, int end, int) {
        switch (source.depth) {
            case PixelDepth::kBits8:
                ExtractRows<PixelDepth::kBits8>(source, offset_x, offset_y, scale, grid_start, threshold, transfer,
                                                level0, begin, end);
                break;
            case PixelDepth::kBits16:
                ExtractRows<PixelDepth::kBits16>(source, offset_x, offset_y, scale, grid_start, threshold,
                                                 transfer, level0, begin, end);
                break;
            case PixelDepth::kFloat32:
                ExtractRows<PixelDepth::kFloat32>(source, offset_x, offset_y, scale, grid_start, threshold,
                                                  transfer, level0, begin, end);
                break;
        }
    });
}

inline float Hash01(std::uint32_t x, std::uint32_t y) {
    std::uint32_t h = x * 374761393u + y * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return static_cast<float>(h & 0xFFFFFFu) * (1.0f / 16777216.0f);
}

// Stochastic rounding: a sample lands on one of the two levels it sits between,
// with probability given by where it falls, so the mean is exact and the
// stair-stepping a wide glow would otherwise show in 8 bpc is broken up. A
// value that is already representable - black, in particular - cannot move,
// which the usual +-1 LSB triangular dither does not guarantee: it rounds an
// exact zero up a quarter of the time, filling the expanded bounds with neutral
// speckle on an otherwise black frame.
inline float DitherOffset(int x, int y) {
    return Hash01(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y)) - 0.5f;
}

// Reconstruction filter for the final upsample out of the pyramid. Bilinear is
// deliberately not offered: it leaves a kink every `base_scale` pixels, which
// reads as concentric rings in a wide, faint glow. Both B-splines have a
// continuous first derivative, so the tail stays smooth.
enum class UpsampleFilter { kQuadratic, kCubic };

// Upsampling taps for one axis. Precomputing them keeps clamping and weight
// math out of the composite's inner loop.
struct AxisTaps {
    int index[4] = {};
    float weight[4] = {};
};

AxisTaps MakeTaps(float coordinate, int limit, UpsampleFilter filter) {
    AxisTaps taps;
    auto clamp_index = [limit](int i) { return i < 0 ? 0 : (i >= limit ? limit - 1 : i); };

    if (filter == UpsampleFilter::kQuadratic) {
        const int centre = static_cast<int>(std::floor(coordinate + 0.5f));
        const float f = coordinate - static_cast<float>(centre);
        taps.weight[0] = 0.5f * (0.5f - f) * (0.5f - f);
        taps.weight[1] = 0.75f - f * f;
        taps.weight[2] = 0.5f * (0.5f + f) * (0.5f + f);
        for (int i = 0; i < 3; ++i) taps.index[i] = clamp_index(centre - 1 + i);
        return taps;
    }

    const int base = static_cast<int>(std::floor(coordinate));
    const float f = coordinate - static_cast<float>(base);
    const float f2 = f * f;
    const float f3 = f2 * f;
    taps.weight[0] = (1.0f - 3.0f * f + 3.0f * f2 - f3) / 6.0f;
    taps.weight[1] = (4.0f - 6.0f * f2 + 3.0f * f3) / 6.0f;
    taps.weight[2] = (1.0f + 3.0f * f + 3.0f * f2 - 3.0f * f3) / 6.0f;
    taps.weight[3] = f3 / 6.0f;
    for (int i = 0; i < 4; ++i) taps.index[i] = clamp_index(base - 1 + i);
    return taps;
}

// One pyramid level the composite reconstructs the glow from: where its grid
// sits in destination pixels, how coarse it is, and which destination pixels
// it covers at all.
struct GlowSource {
    const ImageF* image = nullptr;
    int scale = 1;
    PixelPoint grid_start;
    // Horizontal taps per destination column; identical for every row.
    const AxisTaps* columns = nullptr;
    int x_begin = 0;
    int x_end = 0;
    int y_begin = 0;
    int y_end = 0;
};

struct CompositeContext {
    CompositeMode mode = CompositeMode::kAdd;
    float source_opacity = 1.0f;
    const TransferFunction* transfer = nullptr;
    bool dither = false;
    // Highlight handling. Preserve Hue only runs where the output clips at 1
    // (`bounded`: 8 and 16 bpc); Burn to White runs at every depth and only
    // compresses the range where it is bounded.
    bool rolloff = false;
    bool burn = false;
    bool bounded = false;
    UpsampleFilter filter = UpsampleFilter::kQuadratic;
    int taps = 3;
    // The level-0 collapse. With a single tier it is the whole glow; with two it
    // is the core, and covers only the layer and what the core tier spills.
    GlowSource core;
    // The halo tier's collapse, over the whole reach; no image with one tier.
    GlowSource halo;
};

// The reconstruction filter is separable, and consecutive output rows share
// most of their vertical taps, so each pyramid row is filtered horizontally
// once and kept. That turns taps x taps work per output pixel into taps plus
// taps/scale. Output rows are visited in order within a band, so the window
// only ever moves forward and four slots are enough.
class GlowRowCache {
public:
    GlowRowCache(const GlowSource& source, int taps, int width)
        : source_(source), taps_(taps), width_(width),
          storage_(static_cast<std::size_t>(width) * kSlots) {}

    const PixelF* Row(int glow_y) {
        const int slot = glow_y & (kSlots - 1);
        PixelF* out = storage_.data() + static_cast<std::size_t>(slot) * static_cast<std::size_t>(width_);
        if (row_id_[slot] != glow_y) {
            Filter(glow_y, out);
            row_id_[slot] = glow_y;
        }
        return out;
    }

private:
    void Filter(int glow_y, PixelF* out) const {
        const PixelF* src = source_.image->Row(glow_y);
        for (int x = source_.x_begin; x < source_.x_end; ++x) {
            const AxisTaps& cols = source_.columns[x];
            PixelF acc{0.0f, 0.0f, 0.0f, 0.0f};
            for (int i = 0; i < taps_; ++i) {
                const PixelF& p = src[cols.index[i]];
                const float w = cols.weight[i];
                acc.a += p.a * w;
                acc.r += p.r * w;
                acc.g += p.g * w;
                acc.b += p.b * w;
            }
            out[x] = acc;
        }
    }

    static constexpr int kSlots = 4;
    const GlowSource& source_;
    int taps_ = 3;
    int width_ = 0;
    std::vector<PixelF> storage_;
    int row_id_[kSlots] = {-1, -1, -1, -1};
};

// Reconstructs one GlowSource at destination resolution, a row at a time.
class GlowSampler {
public:
    GlowSampler(const GlowSource& source, const CompositeContext& ctx, int dest_width)
        : source_(source), filter_(ctx.filter), taps_(ctx.taps),
          cache_(source, ctx.taps, source.image != nullptr && source.scale != 1 ? dest_width : 0) {}

    void BeginRow(int y) {
        active_ = source_.image != nullptr && y >= source_.y_begin && y < source_.y_end;
        if (!active_) return;
        if (source_.scale == 1) {
            direct_ = source_.image->Row(y - source_.grid_start.y);
            return;
        }
        const float inv_scale = 1.0f / static_cast<float>(source_.scale);
        row_taps_ = MakeTaps((static_cast<float>(y - source_.grid_start.y) + 0.5f) * inv_scale - 0.5f,
                             source_.image->height, filter_);
        for (int j = 0; j < taps_; ++j) filtered_[j] = cache_.Row(row_taps_.index[j]);
    }

    void Accumulate(int x, PixelF& acc) const {
        if (!active_ || x < source_.x_begin || x >= source_.x_end) return;
        if (source_.scale == 1) {
            const PixelF& p = direct_[x - source_.grid_start.x];
            acc.a += p.a;
            acc.r += p.r;
            acc.g += p.g;
            acc.b += p.b;
            return;
        }
        for (int j = 0; j < taps_; ++j) {
            const PixelF& p = filtered_[j][x];
            const float w = row_taps_.weight[j];
            acc.a += p.a * w;
            acc.r += p.r * w;
            acc.g += p.g * w;
            acc.b += p.b * w;
        }
    }

private:
    const GlowSource& source_;
    UpsampleFilter filter_;
    int taps_ = 3;
    GlowRowCache cache_;
    bool active_ = false;
    const PixelF* direct_ = nullptr;
    const PixelF* filtered_[4] = {};
    AxisTaps row_taps_;
};

inline float Quantize(float value, float max_value, float dither) {
    const float v = std::clamp(value, 0.0f, 1.0f) * max_value + dither + 0.5f;
    return std::clamp(v, 0.0f, max_value);
}

inline void StorePixel8(void* row, int x, const PixelF& encoded, float dither) {
    Pixel8& out = static_cast<Pixel8*>(row)[x];
    out.a = static_cast<std::uint8_t>(Quantize(encoded.a, kMaxChannel8, 0.0f));
    out.r = static_cast<std::uint8_t>(Quantize(encoded.r, kMaxChannel8, dither));
    out.g = static_cast<std::uint8_t>(Quantize(encoded.g, kMaxChannel8, dither));
    out.b = static_cast<std::uint8_t>(Quantize(encoded.b, kMaxChannel8, dither));
}

inline void StorePixel16(void* row, int x, const PixelF& encoded, float dither) {
    Pixel16& out = static_cast<Pixel16*>(row)[x];
    out.a = static_cast<std::uint16_t>(Quantize(encoded.a, kMaxChannel16, 0.0f));
    out.r = static_cast<std::uint16_t>(Quantize(encoded.r, kMaxChannel16, dither));
    out.g = static_cast<std::uint16_t>(Quantize(encoded.g, kMaxChannel16, dither));
    out.b = static_cast<std::uint16_t>(Quantize(encoded.b, kMaxChannel16, dither));
}

// Combines source and glow in linear light and re-encodes for the output depth.
// Identity below the knee, asymptotic to 1 above it, C1 at the join. Used
// wherever a value has to stop at 1 without the result showing where it did.
inline float SoftSaturate(float x, float knee) {
    if (!(x > knee)) return x < 0.0f ? 0.0f : x;
    const float head = 1.0f - knee;
    return knee + head * (1.0f - std::exp(-(x - knee) / head));
}

// Screen only means anything in [0,1]: a + b - a*b turns back down above it and
// goes negative. Saturating the product's terms keeps it monotonic, but a hard
// clamp there puts a kink where the glow crosses 1 - a derivative discontinuity
// along a contour, which draws a visible edge through the glow. Rolling them
// off smoothly leaves the operator exact below the knee and C1 everywhere.
inline float ScreenChannel(float a, float b) {
    constexpr float kKnee = 0.75f;
    return a + b - SoftSaturate(a, kKnee) * SoftSaturate(b, kKnee);
}

inline PixelF CombinePixel(const PixelF& raw_source, const PixelF& glow, const CompositeContext& ctx) {
    const float opacity = ctx.source_opacity;
    const PixelF source_linear = opacity >= 1.0f
                                     ? raw_source
                                     : PixelF{raw_source.a * opacity, raw_source.r * opacity,
                                              raw_source.g * opacity, raw_source.b * opacity};
    PixelF lit{};
    switch (ctx.mode) {
        case CompositeMode::kGlowOnly:
            lit.a = std::clamp(glow.a, 0.0f, 1.0f);
            lit.r = glow.r;
            lit.g = glow.g;
            lit.b = glow.b;
            return lit;
        case CompositeMode::kScreen:
            lit.r = ScreenChannel(source_linear.r, glow.r);
            lit.g = ScreenChannel(source_linear.g, glow.g);
            lit.b = ScreenChannel(source_linear.b, glow.b);
            break;
        case CompositeMode::kAdd:
        default:
            lit.r = source_linear.r + glow.r;
            lit.g = source_linear.g + glow.g;
            lit.b = source_linear.b + glow.b;
            break;
    }
    lit.a = std::clamp(source_linear.a + glow.a * (1.0f - source_linear.a), 0.0f, 1.0f);
    return lit;
}

// Smooth shoulder on the brightest channel, applied to the whole triple so the
// ratios between channels - the hue - are untouched. Identity below the knee,
// asymptotic to 1 above it, C1 at the join. Float output keeps its HDR values
// and never sees this.
inline void RolloffHighlights(float& r, float& g, float& b) {
    constexpr float kKnee = 0.75f;
    const float m = std::max(r, std::max(g, b));
    if (!(m > kKnee)) return;
    const float scale = SoftSaturate(m, kKnee) / m;
    r *= scale;
    g *= scale;
    b *= scale;
}

// A colour driven past what the output can show burns to white, the way an
// over-exposed light does on film or on a sensor: the core of a neon tube is
// white even though its glow is coloured. The shoulder Preserve Hue uses says
// how far past the ceiling the brightest channel went - the light it has to
// take off - and that much overexposure pulls the other channels up towards
// the brightest. Nothing changes below the knee, and the join is C1, so no
// contour is drawn where the burn begins. Unbounded (float) output keeps the
// brightest channel's HDR value and only burns.
inline void BurnHighlights(float& r, float& g, float& b, bool bounded) {
    constexpr float kKnee = 0.75f;
    const float m = std::max(r, std::max(g, b));
    if (!(m > kKnee)) return;
    const float rolled = SoftSaturate(m, kKnee);
    const float white = 1.0f - std::exp(-(m - rolled));
    const float level = bounded ? rolled : m;
    const float scale = level / m;
    r = r * scale + (level - r * scale) * white;
    g = g * scale + (level - g * scale) * white;
    b = b * scale + (level - b * scale) * white;
}

inline void ShapeHighlights(float& r, float& g, float& b, const CompositeContext& ctx) {
    if (ctx.burn) {
        BurnHighlights(r, g, b, ctx.bounded);
    } else if (ctx.rolloff) {
        RolloffHighlights(r, g, b);
    }
}

// Linear working space: the premultiplied result is already what the host
// blends, and what it shows over black is the light itself, so that is what
// the highlight handling works on. Working on the unpremultiplied colour
// instead read the faint tail of a glow over a transparent layer as the full
// brightness of the source's colour at low coverage, and the shoulder dimmed
// it by a tenth though it was nowhere near the ceiling. Where the output is
// bounded, alpha is raised to cover the colour so the pixel stays a valid
// premultiplied one - and, stored straight, a colour that fits.
inline PixelF ShapePremultiplied(const PixelF& lit, const CompositeContext& ctx) {
    float r = lit.r;
    float g = lit.g;
    float b = lit.b;
    ShapeHighlights(r, g, b, ctx);
    PixelF out{lit.a, r, g, b};
    if (ctx.bounded) out.a = std::min(1.0f, std::max(out.a, std::max(r, std::max(g, b))));
    return out;
}

// In an encoded working space After Effects blends premultiplied pixels as
// they stand, so what a pixel shows over black is its premultiplied value. The
// glow is light, and light over black encodes as Encode(light) - not as
// Encode(light / coverage) * coverage, which the concave curve makes darker
// the less coverage there is. Built that way, a glow spilling into a
// transparent layer kept a third of its light at the edge and a tenth of it
// out in the tail. So the result is built as light over black, and its alpha
// is the largest encoded channel: the least coverage that can carry that
// colour, which over anything brighter than black composites like Screen.
// An opaque pixel comes out as Encode(source + glow), exactly as it always has.
inline PixelF ComposeEncoded(const PixelF& raw_source, const PixelF& glow, const CompositeContext& ctx) {
    const TransferFunction& transfer = *ctx.transfer;
    PixelF source{0.0f, 0.0f, 0.0f, 0.0f};
    if (ctx.mode != CompositeMode::kGlowOnly && raw_source.a > kTransparent) {
        const float opacity = ctx.source_opacity;
        source = PixelF{raw_source.a * opacity, raw_source.r * opacity, raw_source.g * opacity,
                        raw_source.b * opacity};
    }
    float r = transfer.Decode(source.r);
    float g = transfer.Decode(source.g);
    float b = transfer.Decode(source.b);
    switch (ctx.mode) {
        case CompositeMode::kGlowOnly:
            r = glow.r;
            g = glow.g;
            b = glow.b;
            break;
        case CompositeMode::kScreen:
            r = ScreenChannel(r, glow.r);
            g = ScreenChannel(g, glow.g);
            b = ScreenChannel(b, glow.b);
            break;
        case CompositeMode::kAdd:
        default:
            r += glow.r;
            g += glow.g;
            b += glow.b;
            break;
    }
    ShapeHighlights(r, g, b, ctx);
    PixelF out{0.0f, transfer.Encode(r), transfer.Encode(g), transfer.Encode(b)};
    out.a = std::clamp(std::max(source.a, std::max(out.r, std::max(out.g, out.b))), 0.0f, 1.0f);
    return out;
}

template <PixelDepth kDepth>
struct HostPixel;

template <>
struct HostPixel<PixelDepth::kBits8> {
    using Type = Pixel8;
};

template <>
struct HostPixel<PixelDepth::kBits16> {
    using Type = Pixel16;
};

template <>
struct HostPixel<PixelDepth::kFloat32> {
    using Type = PixelF;
};

inline bool IsZero(const PixelF& p) {
    return p.a == 0.0f && p.r == 0.0f && p.g == 0.0f && p.b == 0.0f;
}

template <PixelDepth kSrcDepth, PixelDepth kDstDepth>
void CompositeRows(const HostImage& source, int offset_x, int offset_y, const CompositeContext& ctx,
                   const HostImage& dest, int y_begin, int y_end) {
    const TransferFunction& transfer = *ctx.transfer;
    const bool has_source = !source.Empty() && ctx.mode != CompositeMode::kGlowOnly;
    // Copying a pixel through untouched is only exact between buffers that
    // store alpha the same way.
    const bool same_convention = source.alpha == dest.alpha;

    GlowSampler core(ctx.core, ctx, dest.width);
    GlowSampler halo(ctx.halo, ctx, dest.width);

    for (int y = y_begin; y < y_end; ++y) {
        const int src_y = y + offset_y;
        const bool src_row_valid = has_source && src_y >= 0 && src_y < source.height;
        const void* src_row = src_row_valid ? source.ConstRow(src_y) : nullptr;
        void* dst_row = dest.Row(y);
        core.BeginRow(y);
        halo.BeginRow(y);
        const int src_x_begin = src_row_valid ? std::max(0, -offset_x) : dest.width;
        const int src_x_end = src_row_valid ? std::min(dest.width, source.width - offset_x) : dest.width;

        for (int x = 0; x < dest.width; ++x) {
            const int src_x = x + offset_x;
            const bool src_valid = x >= src_x_begin && x < src_x_end;

            PixelF glow_pixel{0.0f, 0.0f, 0.0f, 0.0f};
            core.Accumulate(x, glow_pixel);
            halo.Accumulate(x, glow_pixel);

            if constexpr (kSrcDepth == kDstDepth) {
                // Nothing to add here: keep the original pixel bit-exact.
                if (src_valid && same_convention && ctx.mode != CompositeMode::kGlowOnly && IsZero(glow_pixel)) {
                    static_cast<typename HostPixel<kDstDepth>::Type*>(dst_row)[x] =
                        static_cast<const typename HostPixel<kSrcDepth>::Type*>(src_row)[src_x];
                    continue;
                }
            }

            const PixelF raw = src_valid ? Premultiplied(ReadRowPixel<kSrcDepth>(src_row, src_x), source.alpha)
                                         : PixelF{0.0f, 0.0f, 0.0f, 0.0f};
            PixelF encoded;
            if (transfer.IsIdentity()) {
                encoded = ShapePremultiplied(CombinePixel(raw, glow_pixel, ctx), ctx);
            } else {
                encoded = ComposeEncoded(raw, glow_pixel, ctx);
            }
            encoded = ToHostAlpha(encoded, dest.alpha);

            if constexpr (kDstDepth == PixelDepth::kFloat32) {
                static_cast<PixelF*>(dst_row)[x] = encoded;
            } else {
                // Keyed to source pixels, not to the output buffer: the
                // expanded rect moves with the layer, so a buffer-keyed pattern
                // would crawl over the image as the position animates.
                const float dither = ctx.dither ? DitherOffset(x + offset_x, y + offset_y) : 0.0f;
                if constexpr (kDstDepth == PixelDepth::kBits8) {
                    StorePixel8(dst_row, x, encoded, dither);
                } else {
                    StorePixel16(dst_row, x, encoded, dither);
                }
            }
        }
    }
}

template <PixelDepth kDstDepth>
void CompositeDispatchSource(const HostImage& source, int offset_x, int offset_y, const CompositeContext& ctx,
                             const HostImage& dest, int begin, int end) {
    switch (source.depth) {
        case PixelDepth::kBits8:
            CompositeRows<PixelDepth::kBits8, kDstDepth>(source, offset_x, offset_y, ctx, dest, begin, end);
            break;
        case PixelDepth::kBits16:
            CompositeRows<PixelDepth::kBits16, kDstDepth>(source, offset_x, offset_y, ctx, dest, begin, end);
            break;
        case PixelDepth::kFloat32:
            CompositeRows<PixelDepth::kFloat32, kDstDepth>(source, offset_x, offset_y, ctx, dest, begin, end);
            break;
    }
}

void Composite(const HostImage& source, int offset_x, int offset_y, const CompositeContext& ctx,
               const HostImage& dest, TaskRunner& runner) {
    ParallelRows(runner, dest.height, [&](int begin, int end, int) {
        switch (dest.depth) {
            case PixelDepth::kBits8:
                CompositeDispatchSource<PixelDepth::kBits8>(source, offset_x, offset_y, ctx, dest, begin, end);
                break;
            case PixelDepth::kBits16:
                CompositeDispatchSource<PixelDepth::kBits16>(source, offset_x, offset_y, ctx, dest, begin, end);
                break;
            case PixelDepth::kFloat32:
                CompositeDispatchSource<PixelDepth::kFloat32>(source, offset_x, offset_y, ctx, dest, begin, end);
                break;
        }
    });
}

// Straight copy for the case where the glow contributes nothing; the
// destination can be larger than the source, and the extra area is empty.
void CopySource(const GlowRender& render, TaskRunner& runner) {
    const HostImage& source = render.source;
    const HostImage& dest = render.dest;
    const int pixel_size = dest.depth == PixelDepth::kBits8 ? 4 : (dest.depth == PixelDepth::kBits16 ? 8 : 16);

    ParallelRows(runner, dest.height, [&](int begin, int end, int) {
        for (int y = begin; y < end; ++y) {
            std::uint8_t* dst_row = dest.Row(y);
            std::memset(dst_row, 0, static_cast<std::size_t>(dest.width) * static_cast<std::size_t>(pixel_size));
            if (source.Empty() || source.depth != dest.depth) continue;
            const int src_y = y + render.source_offset_y;
            if (src_y < 0 || src_y >= source.height) continue;
            const int x_begin = std::max(0, -render.source_offset_x);
            const int x_end = std::min(dest.width, source.width - render.source_offset_x);
            if (x_end <= x_begin) continue;
            const std::uint8_t* src_row = source.ConstRow(src_y);
            std::memcpy(dst_row + static_cast<std::ptrdiff_t>(x_begin) * pixel_size,
                        src_row + static_cast<std::ptrdiff_t>(x_begin + render.source_offset_x) * pixel_size,
                        static_cast<std::size_t>(x_end - x_begin) * static_cast<std::size_t>(pixel_size));
        }
    });
}

const TransferFunction& SelectTransfer(WorkingSpace space, PixelDepth depth) {
    switch (space) {
        case WorkingSpace::kLinear: return TransferFunction::Identity();
        case WorkingSpace::kSrgb: return TransferFunction::Srgb();
        case WorkingSpace::kAuto:
        default:
            // 32 bpc projects are the ones that work in linear light.
            return depth == PixelDepth::kFloat32 ? TransferFunction::Identity() : TransferFunction::Srgb();
    }
}

int LevelSize(int base, int level) {
    int size = base;
    for (int i = 0; i < level; ++i) size = (size + 1) / 2;
    return size < 1 ? 1 : size;
}

}  // namespace

float RadiusToSigma(float radius) {
    return std::max(0.0f, radius) * 0.32f;
}

GlowPlan PlanForLayer(const GlowSettings& settings, int layer_width, int layer_height) {
    const float sigma = std::max(RadiusToSigma(settings.radius_x), RadiusToSigma(settings.radius_y));
    if (settings.model == GlowModel::kClassic) {
        // The pyramid has to span everywhere the glow has light, not the
        // rectangle the host happens to be asking for, so the memory budget is
        // measured against that span rather than against the destination.
        const int budget_reach = static_cast<int>(std::ceil(4.5f * sigma));
        const int min_scale = MinimumBaseScale(layer_width + 2 * budget_reach, layer_height + 2 * budget_reach,
                                               settings.quality);
        return MakeGlowPlan(sigma, settings.quality, layer_width, layer_height, min_scale, settings.falloff,
                            settings.aberration_r, settings.aberration_g, settings.aberration_b);
    }
    // The core is a fixed size in the composition, so it follows the render
    // resolution the way the radius does.
    return MakeInverseSquarePlan(sigma, kInverseSquareCoreSigma * std::max(settings.resolution, 0.0f),
                                 settings.quality, layer_width, layer_height, 1, settings.falloff,
                                 settings.aberration_r, settings.aberration_g, settings.aberration_b);
}

float GlowReach(const GlowSettings& settings, int layer_width, int layer_height) {
    if (settings.model == GlowModel::kClassic) {
        const float sigma = RadiusToSigma(std::max(settings.radius_x, settings.radius_y));
        return MakeGlowPlan(sigma, settings.quality, layer_width, layer_height).Reach();
    }
    return PlanForLayer(settings, layer_width, layer_height).Reach();
}

GlowPlan PlanForRender(const GlowSettings& settings, const GlowRender& render) {
    return PlanForLayer(settings, render.source.width, render.source.height);
}

GlowResult RenderGlow(const GlowSettings& settings, const GlowRender& render, Allocator& allocator,
                      TaskRunner& runner) {
    if (render.dest.Empty()) return GlowResult::kInvalidArguments;

    const float gain = std::max(0.0f, settings.intensity) * std::exp2(settings.exposure);
    // The fast path copies bytes, which is only right when both buffers store
    // alpha the same way; otherwise the composite below converts them.
    if (!(gain > 0.0f) && render.source.alpha == render.dest.alpha) {
        // Nothing to add: skip the pyramid entirely.
        if (settings.composite == CompositeMode::kGlowOnly) {
            GlowRender empty = render;
            empty.source = HostImage();
            CopySource(empty, runner);
        } else {
            CopySource(render, runner);
        }
        return GlowResult::kOk;
    }

    const TransferFunction& transfer = SelectTransfer(settings.working_space, render.dest.depth);

    const float sigma_x = RadiusToSigma(settings.radius_x);
    const float sigma_y = RadiusToSigma(settings.radius_y);
    const float sigma = std::max(sigma_x, sigma_y);

    const GlowPlan plan = PlanForRender(settings, render);
    const int scale = plan.base_scale;
    const int count = plan.level_count;
    const int split = std::clamp(plan.split_level, 0, count - 1);
    // One pixel of the halo tier's first level, in render pixels. Both tiers
    // are anchored to multiples of it, so the downsample from one into the
    // other lands on whole pixels.
    const int halo_step = scale << split;

    // In source coordinates, so the grid is anchored to the layer's pixels: it
    // must not move when the radius animates the bounds, nor when the host asks
    // for a different rectangle.
    const int reach = static_cast<int>(std::ceil(plan.Reach()));
    const int low_x = std::min(-reach, render.source_offset_x);
    const int low_y = std::min(-reach, render.source_offset_y);
    const int high_x = std::max(render.source.width + reach, render.source_offset_x + render.dest.width);
    const int high_y = std::max(render.source.height + reach, render.source_offset_y + render.dest.height);
    const PixelPoint halo_origin{FloorToMultiple(low_x, halo_step), FloorToMultiple(low_y, halo_step)};
    const int halo_width = CeilDiv(high_x - halo_origin.x, halo_step);
    const int halo_height = CeilDiv(high_y - halo_origin.y, halo_step);

    // With one tier level 0 spans the whole reach. With two it covers the layer
    // and what the core tier's blurs spill past it, rounded out to whole halo
    // pixels so the last core level pairs up exactly.
    PixelPoint core_origin = halo_origin;
    int level0_width = halo_width;
    int level0_height = halo_height;
    if (split > 0) {
        const int margin = static_cast<int>(std::ceil(4.0f * plan.effective_sigma[split - 1] *
                                                      static_cast<float>(scale))) + 2 * scale;
        core_origin = PixelPoint{FloorToMultiple(-margin, halo_step), FloorToMultiple(-margin, halo_step)};
        level0_width = CeilDiv(render.source.width + margin - core_origin.x, halo_step) << split;
        level0_height = CeilDiv(render.source.height + margin - core_origin.y, halo_step) << split;
    }

    // The rest of the pipeline works in destination pixels.
    const PixelPoint grid_start{core_origin.x - render.source_offset_x, core_origin.y - render.source_offset_y};
    const PixelPoint halo_start{halo_origin.x - render.source_offset_x, halo_origin.y - render.source_offset_y};

    OwnedImageF levels[kMaxPyramidLevels];
    int temp_width = 1;
    int temp_height = 1;
    for (int i = 0; i < count; ++i) {
        const int w = i < split ? LevelSize(level0_width, i) : LevelSize(halo_width, i - split);
        const int h = i < split ? LevelSize(level0_height, i) : LevelSize(halo_height, i - split);
        if (!levels[i].Allocate(allocator, w, h)) return GlowResult::kOutOfMemory;
        temp_width = std::max(temp_width, w);
        temp_height = std::max(temp_height, h);
    }
    OwnedImageF temp;
    if (!temp.Allocate(allocator, temp_width, temp_height)) return GlowResult::kOutOfMemory;

    Threshold threshold;
    threshold.level = transfer.Decode(std::max(0.0f, settings.threshold));
    threshold.knee = threshold.level * std::clamp(settings.threshold_softness, 0.0f, 1.0f);
    threshold.saturation_bias = settings.saturation_bias;
    threshold.unmult = settings.unmult;

    ImageF& level0 = levels[0].View();
    if (render.source.Empty()) {
        ParallelRows(runner, level0.height, [&](int begin, int end, int) {
            for (int y = begin; y < end; ++y) {
                PixelF* row = level0.Row(y);
                for (int x = 0; x < level0.width; ++x) row[x] = PixelF{0.0f, 0.0f, 0.0f, 0.0f};
            }
        });
    } else {
        ExtractHighlights(render.source, render.source_offset_x, render.source_offset_y, scale, grid_start,
                          threshold, transfer, level0, runner);
    }

    // Blur each octave, then feed the next one from it.
    const float sigma_ratio_x = sigma > 0.0f ? sigma_x / sigma : 1.0f;
    const float sigma_ratio_y = sigma > 0.0f ? sigma_y / sigma : 1.0f;
    const BlurKernel kernel_x = BlurKernel::Gaussian(plan.level_sigma * sigma_ratio_x);
    const BlurKernel kernel_y = BlurKernel::Gaussian(plan.level_sigma * sigma_ratio_y);
    for (int i = 0; i < count; ++i) {
        BlurSeparable(levels[i].View(), temp.View(), kernel_x, kernel_y, runner);
        if (i + 1 >= count) continue;
        if (i + 1 == split) {
            DownsampleHalfInto(levels[i].View(), levels[i + 1].View(), (core_origin.x - halo_origin.x) / halo_step,
                               (core_origin.y - halo_origin.y) / halo_step, runner);
        } else {
            DownsampleHalf(levels[i].View(), levels[i + 1].View(), runner);
        }
    }

    // Collapse the octaves back down, weighting each one: the halo tier into
    // its first level, the core tier into level 0.
    auto collapse = [&](int first, int last) {
        ScaleInPlace(levels[last].View(), plan.channel_weights[last], runner);
        for (int i = last - 1; i >= first; --i) {
            UpsampleHalfAccumulate(levels[i + 1].View(), levels[i].View(), plan.channel_weights[i], runner);
        }
    };
    collapse(split, count - 1);
    if (split > 0) collapse(0, split - 1);

    // Exposure, intensity, saturation and tint are linear, so they can run on
    // the pyramid before the final upsample.
    GlowColorTransform color;
    color.gain = gain;
    color.saturation = std::max(0.0f, settings.saturation);
    const float tint = std::clamp(settings.tint_amount, 0.0f, 1.0f);
    color.tint_r = 1.0f + (settings.tint_r - 1.0f) * tint;
    color.tint_g = 1.0f + (settings.tint_g - 1.0f) * tint;
    color.tint_b = 1.0f + (settings.tint_b - 1.0f) * tint;
    auto apply_color = [&](ImageF& image) {
        ParallelRows(runner, image.height, [&](int begin, int end, int) {
            for (int y = begin; y < end; ++y) {
                PixelF* row = image.Row(y);
                for (int x = 0; x < image.width; ++x) color.Apply(row[x]);
            }
        });
    };
    apply_color(level0);
    if (split > 0) apply_color(levels[split].View());

    CompositeContext ctx;
    ctx.mode = settings.composite;
    ctx.transfer = &transfer;
    ctx.dither = settings.dither && render.dest.depth != PixelDepth::kFloat32;
    ctx.source_opacity = std::clamp(settings.source_opacity, 0.0f, 1.0f);
    ctx.bounded = render.dest.depth != PixelDepth::kFloat32;
    ctx.rolloff = settings.rolloff == HighlightRolloff::kPreserveHue && ctx.bounded;
    ctx.burn = settings.rolloff == HighlightRolloff::kBurnToWhite;
    ctx.filter = settings.quality >= Quality::kHigh ? UpsampleFilter::kCubic : UpsampleFilter::kQuadratic;
    ctx.taps = ctx.filter == UpsampleFilter::kCubic ? 4 : 3;

    const int dest_width = render.dest.width;
    const int dest_height = render.dest.height;
    auto describe = [&](GlowSource& out, const ImageF& image, int step, const PixelPoint& start, bool whole,
                        std::vector<AxisTaps>& columns) {
        out.image = &image;
        out.scale = step;
        out.grid_start = start;
        out.x_begin = whole ? 0 : std::clamp(start.x, 0, dest_width);
        out.x_end = whole ? dest_width : std::clamp(start.x + image.width * step, 0, dest_width);
        out.y_begin = whole ? 0 : std::clamp(start.y, 0, dest_height);
        out.y_end = whole ? dest_height : std::clamp(start.y + image.height * step, 0, dest_height);
        if (step == 1) return;
        const float inv_step = 1.0f / static_cast<float>(step);
        columns.resize(static_cast<std::size_t>(dest_width));
        for (int x = 0; x < dest_width; ++x) {
            columns[static_cast<std::size_t>(x)] = MakeTaps(
                (static_cast<float>(x - start.x) + 0.5f) * inv_step - 0.5f, image.width, ctx.filter);
        }
        out.columns = columns.data();
    };
    std::vector<AxisTaps> core_columns;
    std::vector<AxisTaps> halo_columns;
    describe(ctx.core, level0, scale, grid_start, split == 0, core_columns);
    if (split > 0) describe(ctx.halo, levels[split].View(), halo_step, halo_start, true, halo_columns);

    Composite(render.source, render.source_offset_x, render.source_offset_y, ctx, render.dest, runner);
    return GlowResult::kOk;
}

}  // namespace abglow
