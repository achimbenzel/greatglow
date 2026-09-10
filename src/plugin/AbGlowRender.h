#pragma once

#include "SdkIncludes.h"

namespace abglow {

PF_Err SmartPreRender(PF_InData* in_data, PF_OutData* out_data, PF_PreRenderExtra* extra);
PF_Err SmartRender(PF_InData* in_data, PF_OutData* out_data, PF_SmartRenderExtra* extra);

// Fallback path for hosts that do not drive SmartFX.
PF_Err LegacyFrameSetup(PF_InData* in_data, PF_OutData* out_data);
PF_Err LegacyRender(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[], PF_LayerDef* output);

}  // namespace abglow
