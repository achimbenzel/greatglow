#include "GlowPipeline.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

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
inline PixelF ExtractHighlight(const PixelF& linear, const Threshold& threshold) {
    // Premultiplied pixels with no alpha emit no light, whatever RGB they carry.
    if (linear.a <= kTransparent) return PixelF{0.0f, 0.0f, 0.0f, 0.0f};
    const float level = std::max(linear.r, std::max(linear.g, linear.b));
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
void ExtractRows(const HostImage& source, int offset_x, int offset_y, int scale, const Threshold& threshold,
                 const TransferFunction& transfer, ImageF& level0, int y_begin, int y_end) {
    const float inv_samples = 1.0f / static_cast<float>(scale * scale);
    for (int ly = y_begin; ly < y_end; ++ly) {
        PixelF* out = level0.Row(ly);
        for (int lx = 0; lx < level0.width; ++lx) {
            PixelF sum{0.0f, 0.0f, 0.0f, 0.0f};
            for (int sy = 0; sy < scale; ++sy) {
                const int src_y = ly * scale + sy + offset_y;
                if (src_y < 0 || src_y >= source.height) continue;
                const void* row = source.ConstRow(src_y);
                for (int sx = 0; sx < scale; ++sx) {
                    const int src_x = lx * scale + sx + offset_x;
                    if (src_x < 0 || src_x >= source.width) continue;
                    const PixelF raw = ReadRowPixel<kDepth>(row, src_x);
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

void ExtractHighlights(const HostImage& source, int offset_x, int offset_y, int scale, const Threshold& threshold,
                       const TransferFunction& transfer, ImageF& level0, TaskRunner& runner) {
    ParallelRows(runner, level0.height, [&](int begin, int end, int) {
        switch (source.depth) {
            case PixelDepth::kBits8:
                ExtractRows<PixelDepth::kBits8>(source, offset_x, offset_y, scale, threshold, transfer, level0,
                                                begin, end);
                break;
            case PixelDepth::kBits16:
                ExtractRows<PixelDepth::kBits16>(source, offset_x, offset_y, scale, threshold, transfer, level0,
                                                 begin, end);
                break;
            case PixelDepth::kFloat32:
                ExtractRows<PixelDepth::kFloat32>(source, offset_x, offset_y, scale, threshold, transfer, level0,
                                                  begin, end);
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

// Triangular dither of +-1 LSB: removes the stair-stepping a wide glow would
// otherwise show in 8 bpc.
inline float DitherOffset(int x, int y) {
    const float r1 = Hash01(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y));
    const float r2 = Hash01(static_cast<std::uint32_t>(x) + 0x9E3779B9u, static_cast<std::uint32_t>(y) + 0x85EBCA6Bu);
    return r1 + r2 - 1.0f;
}

struct CompositeContext {
    CompositeMode mode = CompositeMode::kAdd;
    const TransferFunction* transfer = nullptr;
    bool dither = false;
    bool smooth_upsample = false;
    int scale = 1;
};

inline PixelF SampleGlow(const ImageF& glow, const CompositeContext& ctx, int x, int y) {
    if (ctx.scale == 1) return glow.At(x, y);
    const float inv = 1.0f / static_cast<float>(ctx.scale);
    const float u = (static_cast<float>(x) + 0.5f) * inv - 0.5f;
    const float v = (static_cast<float>(y) + 0.5f) * inv - 0.5f;
    return ctx.smooth_upsample ? SampleBSpline(glow, u, v) : SampleBilinear(glow, u, v);
}

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
    const bool has_source = !source.Empty();

    for (int y = y_begin; y < y_end; ++y) {
        const int src_y = y + offset_y;
        const bool src_row_valid = has_source && src_y >= 0 && src_y < source.height;
        const void* src_row = src_row_valid ? source.ConstRow(src_y) : nullptr;
        void* dst_row = dest.Row(y);

        for (int x = 0; x < dest.width; ++x) {
            const int src_x = x + offset_x;
            const bool src_valid = src_row_valid && src_x >= 0 && src_x < source.width;

            const PixelF glow_pixel = SampleGlow(glow, ctx, x, y);

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
                const float dither = ctx.dither ? DitherOffset(x, y) : 0.0f;
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

GlowResult RenderGlow(const GlowSettings& settings, const GlowRender& render, Allocator& allocator,
                      TaskRunner& runner) {
    if (render.dest.Empty()) return GlowResult::kInvalidArguments;

    const TransferFunction& transfer = SelectTransfer(settings.working_space, render.dest.depth);

    const float sigma_x = RadiusToSigma(settings.radius_x);
    const float sigma_y = RadiusToSigma(settings.radius_y);
    const float sigma = std::max(sigma_x, sigma_y);

    const GlowPlan plan = MakeGlowPlan(sigma, settings.quality);
    const int scale = plan.base_scale;

    const int level0_width = (render.dest.width + scale - 1) / scale;
    const int level0_height = (render.dest.height + scale - 1) / scale;

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
        ExtractHighlights(render.source, render.source_offset_x, render.source_offset_y, scale, threshold, transfer,
                          level0, runner);
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
    color.gain = std::max(0.0f, settings.intensity) * std::exp2(settings.exposure);
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
    ctx.smooth_upsample = settings.quality >= Quality::kHigh;
    ctx.scale = scale;

    Composite(render.source, render.source_offset_x, render.source_offset_y, level0, ctx, render.dest, runner);
    return GlowResult::kOk;
}

}  // namespace abglow
