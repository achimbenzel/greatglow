#pragma once

#include "Pixel.h"

namespace abglow {

enum class Quality { kDraft = 0, kNormal = 1, kHigh = 2, kBest = 3 };

// How the octaves are weighted, which decides what Radius means.
//
// Inverse Square puts the same light in every octave from a fixed fine core up
// to the radius - equal light per octave is what 1/r^2 is - so the hot core
// hugging the source is always there, and Radius sets how far the light
// reaches, adding light as it grows the way a real veiling glare does.
//
// Classic conserves the light it extracts: the whole kernel scales with the
// radius, so a bigger glow is a dimmer one and the finest octave is a fixed
// fraction of the radius rather than a fixed size.
enum class GlowModel { kInverseSquare = 0, kClassic = 1 };

// Inverse Square needs enough octaves to span from about a pixel to the
// radius, which at a radius of 4000 is a dozen.
constexpr int kMaxPyramidLevels = 14;
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
    // Sigma, in render pixels, that the reach is measured from. Zero means the
    // widest octave; the inverse-square ladder sets it because its widest
    // octave carries little light and would otherwise inflate the bounds.
    float reach_sigma = 0.0f;
    // First level of the halo tier. Levels below it cover only the layer and
    // the little their blurs spill, so the core can stay fine however far the
    // glow reaches; from this level up they cover the whole reach. Zero means a
    // single tier, every level spanning the whole reach.
    int split_level = 0;

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

// Light each octave carries in the inverse-square model at Intensity 100%, as a
// fraction of the extracted light. Calibrated against a reference render of the
// look this model is after, white text whose glow measured 0.3-0.4 of the
// source per octave from about a pixel out to the radius: the default threshold
// extracts 0.79 of white, and 0.79 x 0.45 lands on it.
constexpr float kInverseSquareOctaveGain = 0.45f;

// The finest scale the inverse-square glow carries, in full-resolution pixels.
// It is a fixed size in the composition rather than a fraction of the radius,
// which is what keeps the core the same while the radius animates.
constexpr float kInverseSquareCoreSigma = 0.5f;

// The inverse-square plan. `sigma` is the radius as a sigma and `core_sigma`
// the finest octave, both in render pixels. The pyramid step comes from the
// layer's size and the quality's memory budget alone, never from the radius:
// the core is a fixed size, and a step that changed as the radius animated
// would resample it and make it pop. The reach is paid for by starting the
// halo tier (`split_level`) as far up the ladder as the budget needs instead.
// `min_base_scale` is an extra floor on the step. Unlike the classic plan, the
// weights do not sum to one: every octave between the core and the radius
// carries `kInverseSquareOctaveGain`, so the total grows with the log of the
// radius.
GlowPlan MakeInverseSquarePlan(float sigma, float core_sigma, Quality quality, int layer_width, int layer_height,
                               int min_base_scale = 1, float falloff = 2.0f, float red_scale = 1.0f,
                               float green_scale = 1.0f, float blue_scale = 1.0f);

}  // namespace abglow
