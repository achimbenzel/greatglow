#pragma once

namespace abglow {

enum class Quality { kDraft = 0, kNormal = 1, kHigh = 2, kBest = 3 };

constexpr int kMaxPyramidLevels = 10;

// Multi-scale plan: the glow kernel is a weighted sum of Gaussians, one per
// pyramid octave, which gives a bright core with a long smooth tail and keeps
// cost near-constant as the radius grows.
struct GlowPlan {
    int base_scale = 1;        // pyramid level 0 is 1 / base_scale of render resolution
    int level_count = 1;
    float level_sigma = 1.8f;  // blur sigma applied within each level
    float weights[kMaxPyramidLevels] = {};
    float effective_sigma[kMaxPyramidLevels] = {};  // in level-0 pixels

    // Sigma of the combined multi-scale kernel, in render pixels.
    float EffectiveSigma() const;

    // Distance (in render pixels) beyond which the glow is negligible.
    float Reach() const;
};

// `sigma` is the target glow sigma in render-resolution pixels.
GlowPlan MakeGlowPlan(float sigma, Quality quality, int max_base_scale = 8);

}  // namespace abglow
