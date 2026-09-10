#pragma once

#include "Allocator.h"
#include "GlowPlan.h"
#include "SourceImage.h"
#include "TaskRunner.h"

namespace abglow {

enum class CompositeMode { kAdd = 0, kScreen = 1, kGlowOnly = 2 };
enum class WorkingSpace { kAuto = 0, kLinear = 1, kSrgb = 2 };

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
    bool dither = true;
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

GlowResult RenderGlow(const GlowSettings& settings, const GlowRender& render, Allocator& allocator,
                      TaskRunner& runner);

}  // namespace abglow
