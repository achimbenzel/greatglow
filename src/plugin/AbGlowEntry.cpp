#include "AbGlow.h"
#include "AbGlowParams.h"
#include "AbGlowRender.h"
#include "AbGlowPiPLFlags.h"
#include "SdkIncludes.h"

// The PiPL resource and PF_Cmd_GLOBAL_SETUP must advertise the same behaviour,
// so both read the flags from the generated header.
static_assert(AB_GLOW_OUT_FLAGS == (PF_OutFlag_DEEP_COLOR_AWARE | PF_OutFlag_I_EXPAND_BUFFER),
              "PiPL out flags are out of sync with GlobalSetup");
static_assert(AB_GLOW_OUT_FLAGS2 == (PF_OutFlag2_SUPPORTS_SMART_RENDER | PF_OutFlag2_FLOAT_COLOR_AWARE |
                                     PF_OutFlag2_SUPPORTS_THREADED_RENDERING),
              "PiPL out flags 2 are out of sync with GlobalSetup");

namespace {

PF_Err About(PF_InData* in_data, PF_OutData* out_data) {
    PF_SPRINTF(out_data->return_msg, "%s v%s (%s)\r%s", AB_GLOW_NAME, AB_GLOW_VERSION_STRING,
               AB_GLOW_BUILD_ID, AB_GLOW_DESCRIPTION);
    (void)in_data;
    return PF_Err_NONE;
}

PF_Err GlobalSetup(PF_InData* in_data, PF_OutData* out_data) {
    (void)in_data;
    out_data->my_version = PF_VERSION(AB_GLOW_VERSION_MAJOR, AB_GLOW_VERSION_MINOR, AB_GLOW_VERSION_BUG,
                                      PF_Stage_RELEASE, AB_GLOW_VERSION_BUILD);
    out_data->out_flags = AB_GLOW_OUT_FLAGS;
    out_data->out_flags2 = AB_GLOW_OUT_FLAGS2;
    return PF_Err_NONE;
}

}  // namespace

extern "C" DllExport PF_Err EffectMain(PF_Cmd cmd, PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[],
                                       PF_LayerDef* output, void* extra) {
    PF_Err err = PF_Err_NONE;
    // After Effects is a C host: no exception may cross this boundary.
    try {
        switch (cmd) {
            case PF_Cmd_ABOUT:
                err = About(in_data, out_data);
                break;
            case PF_Cmd_GLOBAL_SETUP:
                err = GlobalSetup(in_data, out_data);
                break;
            case PF_Cmd_PARAMS_SETUP:
                err = abglow::SetupParams(in_data, out_data);
                break;
            case PF_Cmd_FRAME_SETUP:
                err = abglow::LegacyFrameSetup(in_data, out_data);
                break;
            case PF_Cmd_RENDER:
                err = abglow::LegacyRender(in_data, out_data, params, output);
                break;
            case PF_Cmd_SMART_PRE_RENDER:
                err = abglow::SmartPreRender(in_data, out_data, static_cast<PF_PreRenderExtra*>(extra));
                break;
            case PF_Cmd_SMART_RENDER:
                err = abglow::SmartRender(in_data, out_data, static_cast<PF_SmartRenderExtra*>(extra));
                break;
            default:
                break;
        }
    } catch (const std::bad_alloc&) {
        err = PF_Err_OUT_OF_MEMORY;
    } catch (const PF_Err& thrown) {
        err = thrown;
    } catch (...) {
        err = PF_Err_INTERNAL_STRUCT_DAMAGED;
    }
    return err;
}

// PF_REGISTER_EFFECT_EXT2 expands to the SDK's 'eFKT' four character constant.
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmultichar"
#endif

extern "C" DllExport PF_Err PluginDataEntryFunction2(PF_PluginDataPtr inPtr, PF_PluginDataCB2 inPluginDataCallBackPtr,
                                                     SPBasicSuite* inSPBasicSuitePtr, const char* inHostName,
                                                     const char* inHostVersion) {
    (void)inSPBasicSuitePtr;
    (void)inHostName;
    (void)inHostVersion;

    PF_Err result = PF_Err_INVALID_CALLBACK;
    PF_REGISTER_EFFECT_EXT2(inPtr, inPluginDataCallBackPtr, AB_GLOW_NAME, AB_GLOW_MATCH_NAME, AB_GLOW_CATEGORY,
                            AE_RESERVED_INFO, "EffectMain", "");
    return result;
}

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
