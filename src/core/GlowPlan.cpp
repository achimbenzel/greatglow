#include "GlowPlan.h"

#include <algorithm>
#include <cmath>

namespace abglow {
namespace {

// How many octaves the glow spans. This is what makes it a bloom rather than a
// blur: real veiling glare falls off as 1/r^2, and a sum of Gaussians whose
// sigmas double reproduces that when every octave carries the same weight, so
// the finest octaves build a bright tight core and the widest ones the halo.
// Concentrating the weight on one scale instead gives a band-limited blur - the
// source's structure disappears into a flat haze, and a small bright shape ends
// up much dimmer than a large one at the same settings.
int OctaveCount(Quality quality) {
    switch (quality) {
        case Quality::kDraft: return 5;
        case Quality::kNormal: return 6;
        case Quality::kHigh: return 7;
        case Quality::kBest: return 8;
    }
    return 6;
}

// How wide the blur at each level is, in level pixels. Quality picks the octave
// count; this stays fixed so the step between octaves is always a factor of two.
constexpr float kLevelSigma = 1.8f;

// Octaves carried past the radius. Without them the widest octave is the radius
// itself and the profile stops being a power law there, falling off as a
// Gaussian - the glow is gone by 1.5x the radius instead of trailing away.
constexpr int kTailOctaves = 1;

// Veiling glare falls off as a power of the angle: the Stiles-Holladay law used
// in the CIE disability-glare equations is 1/theta^2, which the CIE general
// equation steepens towards 1/theta^3 close in. A sum of Gaussians whose sigmas
// double reproduces 1/r^n when the octave weights go as sigma^(2-n): each
// octave's peak density is w_k / sigma_k^2, and setting that proportional to
// sigma_k^-n is what puts the curve on the law.
// The octave weight exponent that makes the rendered skirt come out at
// `falloff`. Pure sigma^(2-n) would give exactly n, but the cutoff at the
// radius steepens the profile below it too; the correction is a straight-line
// fit to the exponent measured from rendered point sources.
float OctaveWeightExponent(float falloff) {
    return 2.45f - 1.107f * std::clamp(falloff, 1.0f, 3.0f);
}

// Variance the box downsample and the reconstruction filter add, in units of
// the per-level blur's variance times level sigma squared. Taking it back off
// the requested sigma keeps the widest octave on the radius that was asked for.
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

// Level-0 pixels each quality may use.
long long PixelBudget(Quality quality) {
    switch (quality) {
        case Quality::kDraft: return 2LL << 20;
        case Quality::kNormal: return 4LL << 20;
        case Quality::kHigh: return 9LL << 20;
        case Quality::kBest: return 36LL << 20;
    }
    return 4LL << 20;
}

// Per-level blur of the inverse-square ladder, in level pixels. Its rungs are
// not solved onto the radius the way the classic ladder's are: they sit at
// fixed sizes, and the radius moves the weights across them instead. Solving
// them onto the radius would move the finest rung - the core - every time the
// radius crossed an octave, which pops.
constexpr float kInverseSquareLevelSigma = 1.0f;

// Where the inverse-square ladder stops, and where the bounds stop, in units of
// the radius sigma. The density is down to an eighth of its plateau at twice
// the radius, and the bounds are measured from a little past that.
constexpr float kInverseSquareTop = 2.0f;
constexpr float kInverseSquareReach = 2.2f;

// Light per octave of sigma at s = sigma / radius sigma. s^(2-n) is the power
// law - flat, for inverse square - and the Gaussian factor cuts it off at the
// radius: untouched below it, an eighth at twice it, gone by three times it.
double OctaveDensity(double s, double exponent) {
    return std::pow(s, exponent) * std::exp(-0.5 * s * s);
}

// Light between two sigmas, integrated over log2(sigma). Integrating over the
// band each rung stands for, rather than sampling the density at the rung,
// is what keeps the total independent of where the rungs happen to fall.
double OctaveLight(double low, double high, double sigma_r, double exponent) {
    if (!(sigma_r > 0.0)) return 0.0;
    // Past six radii the density is below 1e-7 of its plateau.
    high = std::min(high, 6.0 * sigma_r);
    if (!(high > low) || !(low > 0.0)) return 0.0;
    const double a = std::log2(low / sigma_r);
    const double b = std::log2(high / sigma_r);
    const int steps = std::max(1, static_cast<int>(std::ceil((b - a) * 16.0)));
    const double h = (b - a) / static_cast<double>(steps);
    double sum = 0.0;
    for (int k = 0; k < steps; ++k) sum += OctaveDensity(std::exp2(a + (static_cast<double>(k) + 0.5) * h), exponent);
    return sum * h;
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
    if (reach_sigma > 0.0f) return reach_sigma * 3.2f;
    // The widest octave sets how far the glow carries; the mixture's RMS sigma
    // is much smaller than that once the fine octaves are in it.
    const int top = level_count > 0 ? level_count - 1 : 0;
    return effective_sigma[top] * static_cast<float>(base_scale) * 3.2f;
}

int MinimumBaseScale(int width, int height, Quality quality) {
    const long long budget = PixelBudget(quality);
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

GlowPlan MakeGlowPlan(float sigma, Quality quality, int layer_width, int layer_height, int min_base_scale,
                      float falloff, float red_scale, float green_scale, float blue_scale) {
    GlowPlan plan;

    const float spread = std::sqrt(1.0f + kResamplingVariance / (kLevelSigma * kLevelSigma));
    const float nominal = std::max(sigma, 0.0f) / spread;

    // The widest octave carries the requested sigma, and the ladder runs down
    // from it in halves, so the finest is a fixed fraction of the radius - which
    // is what keeps the glow the same shape at every render resolution.
    const int in_band = std::clamp(OctaveCount(quality), 1, kMaxPyramidLevels - kTailOctaves);
    const float top = CascadeFactor(in_band - 1);

    const int wanted = static_cast<int>(std::lround(nominal / (kLevelSigma * top)));
    const int floor_scale = std::max(min_base_scale, 1);
    const int ceiling_scale = std::max(MaximumBaseScale(sigma, layer_width, layer_height), floor_scale);
    plan.base_scale = std::clamp(std::max(wanted, floor_scale), 1, ceiling_scale);

    // Solve for the per-level blur that lands the widest octave on the radius.
    // Rounding base_scale to an integer moves it a little; shortening the ladder
    // when the blur would be too small to be worth a pass keeps it in range.
    float sigma0 = std::max(0.35f, nominal / static_cast<float>(plan.base_scale));
    int band = in_band;
    while (band > 1 && sigma0 / CascadeFactor(band - 1) < 0.6f) --band;
    plan.level_sigma = sigma0 / CascadeFactor(band - 1);
    plan.level_count = std::min(band + kTailOctaves, kMaxPyramidLevels);

    // Power law over the octaves, cut off at the radius. The law on its own is
    // scale-free - 1/r^n has no characteristic size - so weighting the octaves
    // by it alone left Radius changing the glow's brightness but barely its
    // size. The exponential factor is what gives the radius meaning; below it
    // the profile still follows the law.
    const float exponent = OctaveWeightExponent(falloff);
    float sum = 0.0f;
    const float widest = CascadedSigma(plan.level_sigma, band - 1);
    for (int i = 0; i < plan.level_count; ++i) {
        const float scale = CascadedSigma(plan.level_sigma, i);
        plan.effective_sigma[i] = scale * spread;
        const float ratio = scale / widest;
        const float w = std::pow(ratio, exponent) * std::exp(-ratio);
        plan.weights[i] = w;
        sum += w;
    }
    for (int i = 0; i < plan.level_count; ++i) plan.weights[i] /= sum;

    // A channel scaled by s gets the weights it would have had at radius s,
    // which is the octave ladder shifted by log2(s) - interpolated, since the
    // shift is not a whole number of octaves.
    const float scales[3] = {std::max(red_scale, 0.05f), std::max(green_scale, 0.05f),
                             std::max(blue_scale, 0.05f)};
    float channel_sum[3] = {0.0f, 0.0f, 0.0f};
    for (int i = 0; i < plan.level_count; ++i) {
        for (int c = 0; c < 3; ++c) {
            const float shifted = std::log2(CascadedSigma(plan.level_sigma, i) / scales[c]);
            const float base = std::log2(plan.level_sigma);
            const float pos = std::clamp((shifted - base) / std::log2(2.0f), 0.0f, static_cast<float>(plan.level_count - 1));
            const int lo = static_cast<int>(pos);
            const int hi = std::min(lo + 1, plan.level_count - 1);
            const float f = pos - static_cast<float>(lo);
            const float w = plan.weights[lo] * (1.0f - f) + plan.weights[hi] * f;
            channel_sum[c] += w;
            (c == 0 ? plan.channel_weights[i].r : c == 1 ? plan.channel_weights[i].g : plan.channel_weights[i].b) = w;
        }
        plan.channel_weights[i].a = plan.weights[i];
    }
    for (int i = 0; i < plan.level_count; ++i) {
        if (channel_sum[0] > 0.0f) plan.channel_weights[i].r /= channel_sum[0];
        if (channel_sum[1] > 0.0f) plan.channel_weights[i].g /= channel_sum[1];
        if (channel_sum[2] > 0.0f) plan.channel_weights[i].b /= channel_sum[2];
    }
    return plan;
}

GlowPlan MakeInverseSquarePlan(float sigma, float core_sigma, Quality quality, int layer_width, int layer_height,
                               int min_base_scale, float falloff, float red_scale, float green_scale,
                               float blue_scale) {
    GlowPlan plan;
    const float level_sigma = kInverseSquareLevelSigma;
    const float spread = std::sqrt(1.0f + kResamplingVariance / (level_sigma * level_sigma));
    const float sigma_r = std::max(sigma, 0.0f);
    const float core = std::max(core_sigma, 1.0e-3f);
    const int width = std::max(layer_width, 1);
    const int height = std::max(layer_height, 1);

    // A channel with a larger multiplier has a larger radius, so the ladder has
    // to reach the widest of them.
    const float scales[3] = {std::max(red_scale, 0.05f), std::max(green_scale, 0.05f), std::max(blue_scale, 0.05f)};
    const float widest_scale = std::max(1.0f, std::max(scales[0], std::max(scales[1], scales[2])));
    const float top_sigma = kInverseSquareTop * sigma_r * widest_scale;

    auto rung = [&](int base, int level) {
        return static_cast<float>(base) * CascadedSigma(level_sigma, level) * spread;
    };
    // The layer alone decides the step; it only grows past that if the ladder
    // would otherwise run out of levels, which takes a radius far beyond the
    // slider's range.
    int base = std::clamp(std::max(min_base_scale, MinimumBaseScale(width, height, quality)), 1, kMaxBaseScale);
    int count = 1;
    for (;;) {
        count = 1;
        while (count < kMaxPyramidLevels && rung(base, count - 1) < top_sigma) ++count;
        if (rung(base, count - 1) >= top_sigma || base >= kMaxBaseScale) break;
        ++base;
    }
    plan.base_scale = base;
    plan.level_count = count;
    plan.level_sigma = level_sigma;
    for (int i = 0; i < count; ++i) plan.effective_sigma[i] = CascadedSigma(level_sigma, i) * spread;
    plan.reach_sigma = std::max(kInverseSquareReach * sigma_r * widest_scale, rung(base, 0));

    // The halo tier starts at the first level whose full span fits the budget.
    const long long reach = static_cast<long long>(std::ceil(plan.Reach()));
    const long long span_w = static_cast<long long>(width) + 2 * reach;
    const long long span_h = static_cast<long long>(height) + 2 * reach;
    const long long budget = PixelBudget(quality);
    int split = 0;
    while (split < count - 1) {
        const long long step = static_cast<long long>(base) << split;
        if ((span_w / step + 1) * (span_h / step + 1) <= budget) break;
        ++split;
    }
    plan.split_level = split;

    // Each rung carries the light of the band of scales around it: from the
    // geometric midpoint with the rung below to the one with the rung above.
    // Everything finer than the first rung - the core, which a coarse pyramid
    // cannot resolve - folds into it, and everything past the last folds into
    // that, so the total does not depend on where the grid put the rungs.
    double edges[kMaxPyramidLevels + 1];
    edges[0] = core;
    for (int i = 1; i < count; ++i) edges[i] = std::max<double>(core, std::sqrt(rung(base, i - 1) * rung(base, i)));
    edges[count] = 1.0e30;

    const double exponent = 2.0 - static_cast<double>(std::clamp(falloff, 1.0f, 3.0f));
    // Falloff moves light between the core and the halo; it does not change how
    // much there is. The total is always that of inverse square at this radius.
    const double reference = OctaveLight(core, 1.0e30, sigma_r, 0.0);

    auto fill = [&](double channel_sigma, float* out) {
        const double total = OctaveLight(core, 1.0e30, channel_sigma, exponent);
        const double norm = total > 0.0 ? kInverseSquareOctaveGain * reference / total : 0.0;
        for (int i = 0; i < count; ++i) {
            out[i] = static_cast<float>(norm * OctaveLight(edges[i], edges[i + 1], channel_sigma, exponent));
        }
    };

    fill(sigma_r, plan.weights);
    float channel[3][kMaxPyramidLevels] = {};
    for (int c = 0; c < 3; ++c) fill(sigma_r * scales[c], channel[c]);
    for (int i = 0; i < count; ++i) {
        plan.channel_weights[i] = PixelF{plan.weights[i], channel[0][i], channel[1][i], channel[2][i]};
    }
    return plan;
}

}  // namespace abglow
