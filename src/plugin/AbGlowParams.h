#pragma once

#include "SdkIncludes.h"
#include "core/GlowPipeline.h"

namespace abglow {

// Parameter order is part of the saved project format: from the first shipped
// build of this effect onward, append, never insert or reorder, or projects
// saved by an earlier build open with their values against the wrong
// parameters. The numbers are spelled out so a stray insertion is a compile
// error rather than a corrupted project.
enum ParamIndex {
    kParamInput = 0,
    kParamGlowGroupStart = 1,
    kParamThreshold = 2,
    kParamSoftness = 3,
    kParamRadius = 4,
    kParamIntensity = 5,
    kParamGlowGroupEnd = 6,
    kParamColorGroupStart = 7,
    kParamExposure = 8,
    kParamSaturation = 9,
    kParamTint = 10,
    kParamTintAmount = 11,
    kParamColorGroupEnd = 12,
    kParamRenderGroupStart = 13,
    kParamQuality = 14,
    kParamComposite = 15,
    kParamWorkingSpace = 16,
    kParamExpandBounds = 17,
    kParamRolloff = 18,
    kParamRenderGroupEnd = 19,
    kParamAboutGroupStart = 20,
    kParamAboutGroupEnd = 21,
    kParamFalloff = 22,
    kParamCount = 23
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
