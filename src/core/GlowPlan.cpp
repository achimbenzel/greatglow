#include "GlowPlan.h"

#include <algorithm>
#include <cmath>

namespace abglow {
namespace {

float LevelSigmaForQuality(Quality quality) {
    switch (quality) {
        case Quality::kDraft: return 1.3f;
        case Quality::kNormal: return 1.8f;
        case Quality::kHigh: return 2.3f;
        case Quality::kBest: return 2.8f;
    }
    return 1.8f;
}

// Render pixels per level-0 pixel. Higher quality keeps more resolution.
int BaseScaleForQuality(float sigma, Quality quality, int min_base_scale) {
    float target = sigma / 8.0f;
    switch (quality) {
        case Quality::kDraft: target *= 2.0f; break;
        case Quality::kNormal: break;
        case Quality::kHigh: target *= 0.5f; break;
        case Quality::kBest: target *= 0.25f; break;
    }
    int scale = 1;
    while (scale * 2 <= 8 && static_cast<float>(scale * 2) <= target) scale *= 2;
    return std::max(scale, std::clamp(min_base_scale, 1, 8));
}

// Sigma of the cascaded blurs up to level i, measured in level-0 pixels.
float CascadedSigma(float level_sigma, int level) {
    const float ratio = std::pow(4.0f, static_cast<float>(level + 1));
    return level_sigma * std::sqrt((ratio - 1.0f) / 3.0f);
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
    return EffectiveSigma() * 3.0f;
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
    while (scale < 8 && pixels / (static_cast<long long>(scale) * scale) > budget) scale *= 2;
    return scale;
}

GlowPlan MakeGlowPlan(float sigma, Quality quality, int min_base_scale) {
    GlowPlan plan;
    plan.level_sigma = LevelSigmaForQuality(quality);
    plan.base_scale = BaseScaleForQuality(sigma, quality, min_base_scale);

    const float sigma0 = std::max(0.35f, sigma / static_cast<float>(plan.base_scale));

    // Small radii must not be widened by the per-level blur.
    plan.level_sigma = std::min(plan.level_sigma, sigma0 / 1.155f);

    // Include octaves up to ~1.5x the target so the envelope is not truncated.
    const float top = 1.5f * sigma0 / (plan.level_sigma * 1.155f);
    int levels = 1 + static_cast<int>(std::ceil(std::log2(std::max(1.0f, top))));
    plan.level_count = std::clamp(levels, 1, kMaxPyramidLevels);

    // Log-normal envelope centred on the requested sigma.
    constexpr float kOctaveSpread = 0.8f;
    float sum = 0.0f;
    for (int i = 0; i < plan.level_count; ++i) {
        plan.effective_sigma[i] = CascadedSigma(plan.level_sigma, i);
        const float octaves = std::log2(plan.effective_sigma[i] / sigma0) / kOctaveSpread;
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
