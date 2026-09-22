#pragma once

#include "Allocator.h"
#include "GlowPlan.h"
#include "SourceImage.h"
#include "TaskRunner.h"

namespace abglow {

enum class CompositeMode { kAdd = 0, kScreen = 1, kGlowOnly = 2 };
enum class WorkingSpace { kAuto = 0, kLinear = 1, kSrgb = 2 };

// What to do when the lit result leaves the output's range. Clipping each
// channel on its own drags a saturated colour towards white, because the
// channels reach the ceiling at different brightnesses; rolling the triple off
// together keeps the hue and just stops getting brighter. Burning to white
// does on purpose what clipping does by accident, and smoothly: the more a
// colour is overexposed, the whiter it gets, which is the hot core of a light.
enum class HighlightRolloff { kPreserveHue = 0, kClip = 1, kBurnToWhite = 2 };

struct GlowSettings {
    float threshold = 0.5f;          // linear luminance where the glow starts
    float threshold_softness = 0.4f;  // 0 = hard knee, 1 = very soft
    float radius_x = 30.0f;           // render pixels
    float radius_y = 30.0f;
    float intensity = 1.0f;
    float exposure = 0.0f;  // stops
    float saturation = 1.0f;
    float tint_r = 1.0f;
    float tint_g = 1.0f;
    float tint_b = 1.0f;
    float tint_amount = 0.0f;  // 0 = untinted, 1 = fully tinted
    Quality quality = Quality::kNormal;
    CompositeMode composite = CompositeMode::kAdd;
    WorkingSpace working_space = WorkingSpace::kAuto;
    HighlightRolloff rolloff = HighlightRolloff::kPreserveHue;
    float falloff = 1.4f;  // exponent n of the 1/r^n the glow follows
    float aberration_r = 1.0f;  // per-channel radius scale: lens chromatic aberration
    float aberration_g = 1.0f;
    float aberration_b = 1.0f;
    float saturation_bias = 0.0f;  // pushes the glow's own saturation
    float source_opacity = 1.0f;
    bool unmult = false;
    bool dither = true;
    // Classic here keeps the core's own default where it has always been; the
    // plug-in's parameter defaults to Inverse Square.
    GlowModel model = GlowModel::kClassic;
    // Render pixels per full-resolution pixel. The inverse-square core is a
    // fixed size in the composition, so it has to follow the render resolution
    // the way the radius does.
    float resolution = 1.0f;
};

// Geometry of one render. The glow is generated over the destination rect, so
// `source_offset` says where the destination's origin lands in the source.
struct GlowRender {
    HostImage source;
    HostImage dest;
    int source_offset_x = 0;
    int source_offset_y = 0;
};

enum class GlowResult { kOk, kInvalidArguments, kOutOfMemory };

// Converts the UI radius (0-1000) into a Gaussian sigma in pixels.
float RadiusToSigma(float radius);

// The plan for a layer of this size, in render pixels. Pre-render sizes the
// bounds from it and the render builds the pyramid from it, so both see the
// same plan.
GlowPlan PlanForLayer(const GlowSettings& settings, int layer_width, int layer_height);

// How far past the layer's edges the glow reaches, in render pixels: what
// pre-render expands the bounds by. The classic reach is taken from a plan
// without the memory budget, so switching Quality does not move the bounds;
// the inverse-square plan's step does not depend on the radius, so its reach
// is the render's own.
float GlowReach(const GlowSettings& settings, int layer_width, int layer_height);

// The plan a given render will use. Exposed so a diagnostic build can report
// exactly what the pipeline chose, rather than recomputing it and drifting.
GlowPlan PlanForRender(const GlowSettings& settings, const GlowRender& render);

GlowResult RenderGlow(const GlowSettings& settings, const GlowRender& render, Allocator& allocator,
                      TaskRunner& runner);

}  // namespace abglow
