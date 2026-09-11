#include "GlowPlan.h"

#include <algorithm>
#include <cmath>

namespace abglow {
namespace {

// The rung of the ladder the requested sigma is placed on. Fixing it - rather
// than letting it fall wherever the powers of two happen to put it - is what
// makes the kernel the same shape for every radius, resolution and quality.
// Only the resolution the pyramid is sampled at changes.
constexpr int kCentreRung = 2;

// How wide the blur at each level is, in level pixels. This is the quality
// control: a larger value means a finer pyramid for the same glow, so the
// resampling filters contribute less and the glow's structure is better
// resolved. Cost scales with its square.
float TargetLevelSigma(Quality quality) {
    switch (quality) {
        case Quality::kDraft: return 1.3f;
        case Quality::kNormal: return 1.8f;
        case Quality::kHigh: return 2.3f;
        case Quality::kBest: return 2.8f;
    }
    return 1.8f;
}

// The band the per-level blur is allowed to land in, relative to the target.
// Rungs are a factor of two apart, so this is just wide enough that a rung
// always fits; it only comes into play once base_scale has been clamped.
constexpr float kLevelSigmaLow = 0.7f;
constexpr float kLevelSigmaHigh = 1.5f;

// Variance the box downsample and the reconstruction filter add, in units of
// the per-level blur's variance times target squared - so a coarse pyramid
// widens the glow more than a fine one. Measured by fitting glow width against
// 1 / target squared. Taking it back off the requested sigma is what stops the
// Quality control from changing the size of the glow.
constexpr float kResamplingVariance = 0.32f;

// How much wider level `level` is than the per-level blur, once the blurs of
// every level below it have cascaded in. Tends to 1.155 * 2^level, but is
// noticeably smaller for the first few rungs.
float CascadeFactor(int level) {
    const float ratio = std::pow(4.0f, static_cast<float>(level + 1));
    return std::sqrt((ratio - 1.0f) / 3.0f);
}

// Sigma of the cascaded blurs up to level i, measured in level-0 pixels.
float CascadedSigma(float level_sigma, int level) {
    return level_sigma * CascadeFactor(level);
}

}  // namespace

float GlowPlan::EffectiveSigma() const {
    float variance = 0.0f;
    for (int i = 0; i < level_count; ++i) {
        variance += weights[i] * effective_sigma[i] * effective_sigma[i];
    }
    return std::sqrt(variance) * static_cast<float>(base_scale);
}

float GlowPlan::Reach() const {
    // Far enough out that the glow has fallen below ~1% of its peak; the
    // expanded bounds have to cover everything that is still visible.
    return EffectiveSigma() * 3.6f;
}

int MinimumBaseScale(int width, int height, Quality quality) {
    long long budget = 4LL << 20;
    switch (quality) {
        case Quality::kDraft: budget = 2LL << 20; break;
        case Quality::kNormal: budget = 4LL << 20; break;
        case Quality::kHigh: budget = 9LL << 20; break;
        case Quality::kBest: budget = 36LL << 20; break;
    }
    const long long pixels = static_cast<long long>(width) * static_cast<long long>(height);
    int scale = 1;
    while (scale < kMaxBaseScale && pixels / (static_cast<long long>(scale) * scale) > budget) ++scale;
    return scale;
}

int MaximumBaseScale(float sigma, int layer_width, int layer_height) {
    // Level 0 keeps at least this many pixels on its short side, so the widest
    // octave is still resolved instead of degenerating into edge clamping.
    constexpr int kMinLevelExtent = 48;
    // The pyramid covers the layer plus the glow's reach on each side. Using an
    // approximation of the reach here keeps the limit non-circular; it only has
    // to be proportional, not exact.
    const float working =
        static_cast<float>(std::max(1, std::min(layer_width, layer_height))) + 9.0f * std::max(sigma, 0.0f);
    return std::clamp(static_cast<int>(working / static_cast<float>(kMinLevelExtent)), 1, kMaxBaseScale);
}

GlowPlan MakeGlowPlan(float sigma, Quality quality, int layer_width, int layer_height, int min_base_scale) {
    GlowPlan plan;

    // Pick the pyramid step so the centre rung lands on the requested sigma with
    // the per-level blur the quality asks for. Any integer step works - it is
    // just the size of the box the highlights are averaged over - so the step
    // follows the radius continuously instead of jumping by powers of two.
    const float target = TargetLevelSigma(quality);
    const float spread = std::sqrt(1.0f + kResamplingVariance / (target * target));
    const float nominal = sigma / spread;
    const float desired = target * CascadeFactor(kCentreRung);
    const int wanted = static_cast<int>(std::lround(nominal / desired));
    // The memory floor outranks the resolution ceiling: running out of memory
    // is worse than a coarse top octave.
    const int floor_scale = std::max(min_base_scale, 1);
    const int ceiling_scale = std::max(MaximumBaseScale(sigma, layer_width, layer_height), floor_scale);
    plan.base_scale = std::clamp(std::max(wanted, floor_scale), 1, ceiling_scale);

    const float sigma0 = std::max(0.35f, nominal / static_cast<float>(plan.base_scale));

    // Solve for the per-level blur that puts rung `centre` on the requested
    // sigma, so every plan samples the envelope at the same offsets. The rung
    // only moves if base_scale hit a limit - a tiny radius, a huge one, or the
    // level-0 pixel budget.
    int centre = kCentreRung;
    while (centre > 0 && sigma0 / CascadeFactor(centre) < target * kLevelSigmaLow) --centre;
    while (centre < kMaxPyramidLevels - 2 && sigma0 / CascadeFactor(centre) > target * kLevelSigmaHigh) ++centre;
    plan.level_sigma = sigma0 / CascadeFactor(centre);

    // Only one octave above the centre. The envelope still has weight further
    // out, but an octave that wide is comparable to the whole working buffer,
    // so its blur degenerates into edge clamping - and how degenerate it is
    // depends on the frame size, which made the same glow come out different
    // sizes at different render resolutions. Everything kept here is resolved
    // by a wide margin.
    plan.level_count = std::clamp(centre + 2, 1, kMaxPyramidLevels);

    // Log-normal envelope centred on the requested sigma.
    constexpr float kOctaveSpread = 0.8f;
    float sum = 0.0f;
    for (int i = 0; i < plan.level_count; ++i) {
        // Inflated by `spread`, so EffectiveSigma and Reach describe the glow
        // the filters actually produce rather than the blur they were given.
        plan.effective_sigma[i] = CascadedSigma(plan.level_sigma, i) * spread;
        const float octaves = std::log2(plan.effective_sigma[i] / (sigma0 * spread)) / kOctaveSpread;
        const float w = std::exp(-0.5f * octaves * octaves);
        plan.weights[i] = w;
        sum += w;
    }
    if (sum <= 0.0f) {
        plan.weights[0] = 1.0f;
    } else {
        for (int i = 0; i < plan.level_count; ++i) plan.weights[i] /= sum;
    }
    return plan;
}

}  // namespace abglow
