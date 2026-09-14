#pragma once

#include "Pixel.h"

namespace abglow {

enum class Quality { kDraft = 0, kNormal = 1, kHigh = 2, kBest = 3 };

constexpr int kMaxPyramidLevels = 10;
constexpr int kMaxBaseScale = 128;

// Multi-scale plan: the glow kernel is a weighted sum of Gaussians, one per
// pyramid octave, which gives a bright core with a long smooth tail and keeps
// cost near-constant as the radius grows.
struct GlowPlan {
    int base_scale = 1;        // pyramid level 0 is 1 / base_scale of render resolution
    int level_count = 1;
    float level_sigma = 1.8f;  // blur sigma applied within each level
    float weights[kMaxPyramidLevels] = {};
    // Per-channel weights. Scaling a channel's octave weights towards the wide
    // end is the same as giving it a larger radius, which is what chromatic
    // aberration in a lens does.
    PixelF channel_weights[kMaxPyramidLevels] = {};
    float effective_sigma[kMaxPyramidLevels] = {};  // in level-0 pixels

    // Sigma of the combined multi-scale kernel, in render pixels.
    float EffectiveSigma() const;

    // Distance (in render pixels) beyond which the glow is negligible.
    float Reach() const;
};

// Smallest pyramid step that keeps the level-0 buffer within the quality
// setting's memory budget. Large frames start the pyramid lower down, which is
// what keeps a 4K render from allocating hundreds of megabytes per frame.
int MinimumBaseScale(int width, int height, Quality quality);

// Largest pyramid step that still leaves the top of the ladder resolved, for a
// glow of `sigma` over a layer of this size. Derived from the working buffer
// rather than fixed, so it scales with the render resolution.
int MaximumBaseScale(float sigma, int layer_width, int layer_height);

// `sigma` is the target glow sigma in render-resolution pixels; the layer size
// is in the same pixels.
// `falloff` is the exponent n of the 1/r^n the glow's skirt follows. 2 is the
// Stiles-Holladay inverse-square law of real veiling glare; lower puts more
// light at the wide scales, which is what gives Radius authority over the
// glow's apparent size.
GlowPlan MakeGlowPlan(float sigma, Quality quality, int layer_width, int layer_height, int min_base_scale = 1,
                      float falloff = 1.4f, float red_scale = 1.0f,
                      float green_scale = 1.0f, float blue_scale = 1.0f);

}  // namespace abglow
