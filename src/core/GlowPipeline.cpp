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
    if (linear.a <= kTransparent) return PixelF{0.0f, 0.0f, 0.0f, 0.0f};
    const float inv_coverage = linear.a >= kOpaque ? 1.0f : 1.0f / linear.a;
    const float level = std::max(linear.r, std::max(linear.g, linear.b)) * inv_coverage;
    if (level <= 0.0f) return PixelF{0.0f, 0.0f, 0.0f, 0.0f};

    float above = level - threshold.level;
    if (threshold.knee > 0.0f) {
        float soft = std::clamp(above + threshold.knee, 0.0f, 2.0f * threshold.knee);
        soft = soft * soft / (4.0f * threshold.knee);
        above = std::max(soft, above);
    }
    if (above <= 0.0f) return PixelF{0.0f, 0.0f, 0.0f, 0.0f};

    const float contribution = above / std::max(level, 1.0e-6f);
    return PixelF{linear.a * contribution, linear.r * contribution, linear.g * contribution,
                  linear.b * contribution};
}

template <PixelDepth kDepth>
void ExtractRows(const HostImage& source, int offset_x, int offset_y, int scale, const PixelPoint& grid_start,
                 const Threshold& threshold, const TransferFunction& transfer, ImageF& level0, int y_begin,
                 int y_end) {
    const float inv_samples = 1.0f / static_cast<float>(scale * scale);
    // Source x of level-0 column lx is lx * scale + block_offset_x.
    const int block_offset_x = grid_start.x + offset_x;
    const int block_offset_y = grid_start.y + offset_y;
    // Columns whose whole block lies inside the source image, and the wider
    // range of columns that touch it at all. The partial columns at the far
    // edge have to be summed too: dropping them would throw away up to
    // `scale` columns of real image, which moves the glow sideways whenever
    // the radius changes `scale`.
    const int full_begin = std::clamp(CeilDiv(-block_offset_x, scale), 0, level0.width);
    const int full_end = std::clamp((source.width - block_offset_x) / scale, full_begin, level0.width);
    const int any_begin = std::clamp(CeilDiv(-block_offset_x - scale + 1, scale), 0, full_begin);
    const int any_end = std::clamp(CeilDiv(source.width - block_offset_x, scale), full_end, level0.width);

    for (int ly = y_begin; ly < y_end; ++ly) {
        PixelF* out = level0.Row(ly);
        const int block_y = ly * scale + block_offset_y;
        const bool rows_inside = block_y >= 0 && block_y + scale <= source.height;

        for (int lx = 0; lx < any_begin; ++lx) out[lx] = PixelF{0.0f, 0.0f, 0.0f, 0.0f};
        for (int lx = any_end; lx < level0.width; ++lx) out[lx] = PixelF{0.0f, 0.0f, 0.0f, 0.0f};

        // Blocks that hang over an edge: accumulate only the samples that exist.
        auto clipped_blocks = [&](int lx_begin, int lx_end) {
            for (int lx = lx_begin; lx < lx_end; ++lx) {
                PixelF sum{0.0f, 0.0f, 0.0f, 0.0f};
                for (int sy = 0; sy < scale; ++sy) {
                    const int src_y = block_y + sy;
                    if (src_y < 0 || src_y >= source.height) continue;
                    const void* row = source.ConstRow(src_y);
                    for (int sx = 0; sx < scale; ++sx) {
                        const int src_x = lx * scale + sx + block_offset_x;
                        if (src_x < 0 || src_x >= source.width) continue;
                        const PixelF raw = ReadRowPixel<kDepth>(row, src_x);
                        const PixelF highlight =
                            ExtractHighlight(LinearizePremultiplied(raw, transfer), threshold);
                        sum.a += highlight.a;
                        sum.r += highlight.r;
                        sum.g += highlight.g;
                        sum.b += highlight.b;
                    }
                }
                out[lx] = PixelF{sum.a * inv_samples, sum.r * inv_samples, sum.g * inv_samples,
                                 sum.b * inv_samples};
            }
        };

        if (!rows_inside) {
            clipped_blocks(any_begin, any_end);
            continue;
        }
        clipped_blocks(any_begin, full_begin);
        clipped_blocks(full_end, any_end);

        if (scale == 1) {
            const void* row = source.ConstRow(block_y);
            for (int lx = full_begin; lx < full_end; ++lx) {
                const PixelF raw = ReadRowPixel<kDepth>(row, lx + block_offset_x);
                out[lx] = ExtractHighlight(LinearizePremultiplied(raw, transfer), threshold);
            }
            continue;
        }

        for (int lx = full_begin; lx < full_end; ++lx) {
            PixelF sum{0.0f, 0.0f, 0.0f, 0.0f};
            for (int sy = 0; sy < scale; ++sy) {
                const void* row = source.ConstRow(block_y + sy);
                for (int sx = 0; sx < scale; ++sx) {
                    const PixelF raw = ReadRowPixel<kDepth>(row, lx * scale + sx + block_offset_x);
                    const PixelF highlight = ExtractHighlight(LinearizePremultiplied(raw, transfer), threshold);
                    sum.a += highlight.a;
                    sum.r += highlight.r;
                    sum.g += highlight.g;
                    sum.b += highlight.b;
                }
            }
            out[lx] = PixelF{sum.a * inv_samples, sum.r * inv_samples, sum.g * inv_samples, sum.b * inv_samples};
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

struct CompositeContext {
    CompositeMode mode = CompositeMode::kAdd;
    const TransferFunction* transfer = nullptr;
    bool dither = false;
    UpsampleFilter filter = UpsampleFilter::kQuadratic;
    int scale = 1;
    int taps = 3;
    PixelPoint grid_start;
    // Horizontal taps per destination column; identical for every row.
    const AxisTaps* columns = nullptr;
};

// The reconstruction filter is separable, and consecutive output rows share
// most of their vertical taps, so each pyramid row is filtered horizontally
// once and kept. That turns taps x taps work per output pixel into taps plus
// taps/scale. Output rows are visited in order within a band, so the window
// only ever moves forward and four slots are enough.
class GlowRowCache {
public:
    GlowRowCache(const ImageF& glow, const CompositeContext& ctx, int width)
        : glow_(glow), ctx_(ctx), width_(width),
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
        const PixelF* src = glow_.Row(glow_y);
        const int taps = ctx_.taps;
        for (int x = 0; x < width_; ++x) {
            const AxisTaps& cols = ctx_.columns[x];
            PixelF acc{0.0f, 0.0f, 0.0f, 0.0f};
            for (int i = 0; i < taps; ++i) {
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
    const ImageF& glow_;
    const CompositeContext& ctx_;
    int width_ = 0;
    std::vector<PixelF> storage_;
    int row_id_[kSlots] = {-1, -1, -1, -1};
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
inline PixelF CombinePixel(const PixelF& source_linear, const PixelF& glow, const CompositeContext& ctx) {
    PixelF lit{};
    switch (ctx.mode) {
        case CompositeMode::kGlowOnly:
            lit.a = std::clamp(glow.a, 0.0f, 1.0f);
            lit.r = glow.r;
            lit.g = glow.g;
            lit.b = glow.b;
            return lit;
        case CompositeMode::kScreen:
            lit.r = source_linear.r + glow.r - source_linear.r * glow.r;
            lit.g = source_linear.g + glow.g - source_linear.g * glow.g;
            lit.b = source_linear.b + glow.b - source_linear.b * glow.b;
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

inline PixelF EncodePremultiplied(const PixelF& linear, const TransferFunction& transfer) {
    if (transfer.IsIdentity()) return linear;
    if (linear.a >= kOpaque) {
        return PixelF{linear.a, transfer.Encode(linear.r), transfer.Encode(linear.g), transfer.Encode(linear.b)};
    }
    if (linear.a <= kTransparent) return PixelF{linear.a, 0.0f, 0.0f, 0.0f};
    const float inv = 1.0f / linear.a;
    return PixelF{linear.a, transfer.Encode(linear.r * inv) * linear.a,
                  transfer.Encode(linear.g * inv) * linear.a, transfer.Encode(linear.b * inv) * linear.a};
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
void CompositeRows(const HostImage& source, int offset_x, int offset_y, const ImageF& glow,
                   const CompositeContext& ctx, const HostImage& dest, int y_begin, int y_end) {
    const TransferFunction& transfer = *ctx.transfer;
    const bool has_source = !source.Empty() && ctx.mode != CompositeMode::kGlowOnly;
    const float inv_scale = 1.0f / static_cast<float>(ctx.scale);

    GlowRowCache cache(glow, ctx, ctx.scale == 1 ? 0 : dest.width);

    for (int y = y_begin; y < y_end; ++y) {
        const int src_y = y + offset_y;
        const bool src_row_valid = has_source && src_y >= 0 && src_y < source.height;
        const void* src_row = src_row_valid ? source.ConstRow(src_y) : nullptr;
        void* dst_row = dest.Row(y);
        const PixelF* filtered[4] = {};
        AxisTaps row_taps;
        if (ctx.scale != 1) {
            row_taps = MakeTaps((static_cast<float>(y - ctx.grid_start.y) + 0.5f) * inv_scale - 0.5f, glow.height,
                                ctx.filter);
            for (int j = 0; j < ctx.taps; ++j) filtered[j] = cache.Row(row_taps.index[j]);
        }
        const int src_x_begin = src_row_valid ? std::max(0, -offset_x) : dest.width;
        const int src_x_end = src_row_valid ? std::min(dest.width, source.width - offset_x) : dest.width;

        for (int x = 0; x < dest.width; ++x) {
            const int src_x = x + offset_x;
            const bool src_valid = x >= src_x_begin && x < src_x_end;

            PixelF glow_pixel{0.0f, 0.0f, 0.0f, 0.0f};
            if (ctx.scale == 1) {
                glow_pixel = glow.At(x - ctx.grid_start.x, y - ctx.grid_start.y);
            } else {
                for (int j = 0; j < ctx.taps; ++j) {
                    const PixelF& p = filtered[j][x];
                    const float w = row_taps.weight[j];
                    glow_pixel.a += p.a * w;
                    glow_pixel.r += p.r * w;
                    glow_pixel.g += p.g * w;
                    glow_pixel.b += p.b * w;
                }
            }

            if constexpr (kSrcDepth == kDstDepth) {
                // Nothing to add here: keep the original pixel bit-exact.
                if (src_valid && ctx.mode != CompositeMode::kGlowOnly && IsZero(glow_pixel)) {
                    static_cast<typename HostPixel<kDstDepth>::Type*>(dst_row)[x] =
                        static_cast<const typename HostPixel<kSrcDepth>::Type*>(src_row)[src_x];
                    continue;
                }
            }

            const PixelF raw = src_valid ? ReadRowPixel<kSrcDepth>(src_row, src_x) : PixelF{0.0f, 0.0f, 0.0f, 0.0f};
            const PixelF source_linear = LinearizePremultiplied(raw, transfer);
            const PixelF lit = CombinePixel(source_linear, glow_pixel, ctx);
            const PixelF encoded = EncodePremultiplied(lit, transfer);

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
void CompositeDispatchSource(const HostImage& source, int offset_x, int offset_y, const ImageF& glow,
                             const CompositeContext& ctx, const HostImage& dest, int begin, int end) {
    switch (source.depth) {
        case PixelDepth::kBits8:
            CompositeRows<PixelDepth::kBits8, kDstDepth>(source, offset_x, offset_y, glow, ctx, dest, begin, end);
            break;
        case PixelDepth::kBits16:
            CompositeRows<PixelDepth::kBits16, kDstDepth>(source, offset_x, offset_y, glow, ctx, dest, begin, end);
            break;
        case PixelDepth::kFloat32:
            CompositeRows<PixelDepth::kFloat32, kDstDepth>(source, offset_x, offset_y, glow, ctx, dest, begin, end);
            break;
    }
}

void Composite(const HostImage& source, int offset_x, int offset_y, const ImageF& glow,
               const CompositeContext& ctx, const HostImage& dest, TaskRunner& runner) {
    ParallelRows(runner, dest.height, [&](int begin, int end, int) {
        switch (dest.depth) {
            case PixelDepth::kBits8:
                CompositeDispatchSource<PixelDepth::kBits8>(source, offset_x, offset_y, glow, ctx, dest, begin, end);
                break;
            case PixelDepth::kBits16:
                CompositeDispatchSource<PixelDepth::kBits16>(source, offset_x, offset_y, glow, ctx, dest, begin,
                                                             end);
                break;
            case PixelDepth::kFloat32:
                CompositeDispatchSource<PixelDepth::kFloat32>(source, offset_x, offset_y, glow, ctx, dest, begin,
                                                              end);
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

GlowPlan PlanForRender(const GlowSettings& settings, const GlowRender& render) {
    // The pyramid has to span everywhere the glow has light, not the rectangle
    // the host happens to be asking for, so the memory budget is measured
    // against that span rather than against the destination.
    const float sigma = std::max(RadiusToSigma(settings.radius_x), RadiusToSigma(settings.radius_y));
    const int budget_reach = static_cast<int>(std::ceil(4.5f * sigma));
    const int min_scale = MinimumBaseScale(render.source.width + 2 * budget_reach,
                                           render.source.height + 2 * budget_reach, settings.quality);
    return MakeGlowPlan(sigma, settings.quality, render.source.width, render.source.height, min_scale);
}

GlowResult RenderGlow(const GlowSettings& settings, const GlowRender& render, Allocator& allocator,
                      TaskRunner& runner) {
    if (render.dest.Empty()) return GlowResult::kInvalidArguments;

    const float gain = std::max(0.0f, settings.intensity) * std::exp2(settings.exposure);
    if (!(gain > 0.0f)) {
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

    // In source coordinates, so the grid is anchored to the layer's pixels: it
    // must not move when the radius animates the bounds, nor when the host asks
    // for a different rectangle.
    const int reach = static_cast<int>(std::ceil(plan.Reach()));
    const int low_x = std::min(-reach, render.source_offset_x);
    const int low_y = std::min(-reach, render.source_offset_y);
    const int high_x = std::max(render.source.width + reach, render.source_offset_x + render.dest.width);
    const int high_y = std::max(render.source.height + reach, render.source_offset_y + render.dest.height);
    const PixelPoint origin{FloorToMultiple(low_x, scale), FloorToMultiple(low_y, scale)};
    const int level0_width = CeilDiv(high_x - origin.x, scale);
    const int level0_height = CeilDiv(high_y - origin.y, scale);

    // The rest of the pipeline works in destination pixels.
    const PixelPoint grid_start{origin.x - render.source_offset_x, origin.y - render.source_offset_y};

    OwnedImageF levels[kMaxPyramidLevels];
    for (int i = 0; i < plan.level_count; ++i) {
        const int w = LevelSize(level0_width, i);
        const int h = LevelSize(level0_height, i);
        if (!levels[i].Allocate(allocator, w, h)) return GlowResult::kOutOfMemory;
    }
    OwnedImageF temp;
    if (!temp.Allocate(allocator, level0_width, level0_height)) return GlowResult::kOutOfMemory;

    Threshold threshold;
    threshold.level = transfer.Decode(std::max(0.0f, settings.threshold));
    threshold.knee = threshold.level * std::clamp(settings.threshold_softness, 0.0f, 1.0f);

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
    for (int i = 0; i < plan.level_count; ++i) {
        BlurSeparable(levels[i].View(), temp.View(), kernel_x, kernel_y, runner);
        if (i + 1 < plan.level_count) {
            DownsampleHalf(levels[i].View(), levels[i + 1].View(), runner);
        }
    }

    // Collapse the octaves back down, weighting each one.
    const int top = plan.level_count - 1;
    ScaleInPlace(levels[top].View(), plan.weights[top], runner);
    for (int i = top - 1; i >= 0; --i) {
        UpsampleHalfAccumulate(levels[i + 1].View(), levels[i].View(), plan.weights[i], runner);
    }

    // Exposure, intensity, saturation and tint are linear, so they can run on
    // the pyramid before the final upsample.
    GlowColorTransform color;
    color.gain = gain;
    color.saturation = std::max(0.0f, settings.saturation);
    const float tint = std::clamp(settings.tint_amount, 0.0f, 1.0f);
    color.tint_r = 1.0f + (settings.tint_r - 1.0f) * tint;
    color.tint_g = 1.0f + (settings.tint_g - 1.0f) * tint;
    color.tint_b = 1.0f + (settings.tint_b - 1.0f) * tint;
    ParallelRows(runner, level0.height, [&](int begin, int end, int) {
        for (int y = begin; y < end; ++y) {
            PixelF* row = level0.Row(y);
            for (int x = 0; x < level0.width; ++x) color.Apply(row[x]);
        }
    });

    CompositeContext ctx;
    ctx.mode = settings.composite;
    ctx.transfer = &transfer;
    ctx.dither = settings.dither && render.dest.depth != PixelDepth::kFloat32;
    ctx.filter = settings.quality >= Quality::kHigh ? UpsampleFilter::kCubic : UpsampleFilter::kQuadratic;
    ctx.scale = scale;
    ctx.grid_start = grid_start;

    std::vector<AxisTaps> columns;
    ctx.taps = ctx.filter == UpsampleFilter::kCubic ? 4 : 3;
    if (scale != 1) {
        const float inv_scale = 1.0f / static_cast<float>(scale);
        columns.resize(static_cast<std::size_t>(render.dest.width));
        for (int x = 0; x < render.dest.width; ++x) {
            columns[static_cast<std::size_t>(x)] = MakeTaps(
                (static_cast<float>(x - grid_start.x) + 0.5f) * inv_scale - 0.5f, level0.width, ctx.filter);
        }
        ctx.columns = columns.data();
    }

    Composite(render.source, render.source_offset_x, render.source_offset_y, level0, ctx, render.dest, runner);
    return GlowResult::kOk;
}

}  // namespace abglow
