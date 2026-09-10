#pragma once

#include "SdkIncludes.h"
#include "core/GlowPipeline.h"

namespace abglow {

// Parameter order is part of the saved project format: append, never reorder.
enum ParamIndex {
    kParamInput = 0,
    kParamGlowGroupStart,
    kParamThreshold,
    kParamSoftness,
    kParamRadius,
    kParamIntensity,
    kParamGlowGroupEnd,
    kParamColorGroupStart,
    kParamExposure,
    kParamSaturation,
    kParamTint,
    kParamTintAmount,
    kParamColorGroupEnd,
    kParamRenderGroupStart,
    kParamQuality,
    kParamComposite,
    kParamWorkingSpace,
    kParamExpandBounds,
    kParamRenderGroupEnd,
    kParamCount
};

PF_Err SetupParams(PF_InData* in_data, PF_OutData* out_data);

struct EffectParams {
    GlowSettings settings;
    bool expand_bounds = true;
};

// Reads every parameter at `in_data->current_time` and converts UI units into
// render-resolution units.
PF_Err ReadParams(PF_InData* in_data, PF_OutData* out_data, EffectParams* out_params);

}  // namespace abglow
