#include "AbGlowParams.h"

#include "AbGlow.h"
#include "AeAdapters.h"

#include <algorithm>
#include <cmath>

namespace abglow {
namespace {

// Parameter ids are stable identifiers used by After Effects when a project is
// reopened; they must never change once shipped.
enum ParamId {
    kIdGlowGroup = 1,
    kIdThreshold,
    kIdSoftness,
    kIdRadius,
    kIdIntensity,
    kIdGlowGroupEnd,
    kIdColorGroup,
    kIdExposure,
    kIdSaturation,
    kIdTint,
    kIdTintAmount,
    kIdColorGroupEnd,
    kIdRenderGroup,
    kIdQuality,
    kIdComposite,
    kIdWorkingSpace,
    kIdExpandBounds,
    kIdRenderGroupEnd,
    kIdAboutGroup,
    kIdAboutGroupEnd
};

constexpr char kQualityChoices[] = "Draft|Normal|High|Best";
constexpr char kCompositeChoices[] = "Add|Screen|Glow Only";
constexpr char kWorkingSpaceChoices[] = "Auto|Linear|sRGB";

float RationalToFloat(const PF_RationalScale& value) {
    if (value.den == 0) return 1.0f;
    return static_cast<float>(value.num) / static_cast<float>(value.den);
}

PF_Err CheckoutFloat(PF_InData* in_data, int index, float* out_value) {
    PF_ParamDef param;
    AEFX_CLR_STRUCT(param);
    PF_Err err = PF_CHECKOUT_PARAM(in_data, index, in_data->current_time, in_data->time_step, in_data->time_scale,
                                   &param);
    if (!err) *out_value = static_cast<float>(param.u.fs_d.value);
    PF_CHECKIN_PARAM(in_data, &param);
    return err;
}

PF_Err CheckoutPopup(PF_InData* in_data, int index, int* out_value) {
    PF_ParamDef param;
    AEFX_CLR_STRUCT(param);
    PF_Err err = PF_CHECKOUT_PARAM(in_data, index, in_data->current_time, in_data->time_step, in_data->time_scale,
                                   &param);
    if (!err) *out_value = static_cast<int>(param.u.pd.value);
    PF_CHECKIN_PARAM(in_data, &param);
    return err;
}

PF_Err CheckoutCheckbox(PF_InData* in_data, int index, bool* out_value) {
    PF_ParamDef param;
    AEFX_CLR_STRUCT(param);
    PF_Err err = PF_CHECKOUT_PARAM(in_data, index, in_data->current_time, in_data->time_step, in_data->time_scale,
                                   &param);
    if (!err) *out_value = param.u.bd.value != 0;
    PF_CHECKIN_PARAM(in_data, &param);
    return err;
}

// Colour params are 8 bit in the UI; the float suite returns them in the
// working colour space, which is what the glow needs.
PF_Err CheckoutColor(PF_InData* in_data, int index, float* out_rgb) {
    PF_ParamDef param;
    AEFX_CLR_STRUCT(param);
    PF_Err err = PF_CHECKOUT_PARAM(in_data, index, in_data->current_time, in_data->time_step, in_data->time_scale,
                                   &param);
    if (!err) {
        bool converted = false;
        PF_ColorParamSuite1* color_suite =
            AcquireSuite<PF_ColorParamSuite1>(in_data->pica_basicP, kPFColorParamSuite, kPFColorParamSuiteVersion1);
        if (color_suite != nullptr) {
            PF_PixelFloat float_color = {};
            if (color_suite->PF_GetFloatingPointColorFromColorDef(in_data->effect_ref, &param, &float_color) ==
                PF_Err_NONE) {
                out_rgb[0] = static_cast<float>(float_color.red);
                out_rgb[1] = static_cast<float>(float_color.green);
                out_rgb[2] = static_cast<float>(float_color.blue);
                converted = true;
            }
            in_data->pica_basicP->ReleaseSuite(kPFColorParamSuite, kPFColorParamSuiteVersion1);
        }
        if (!converted) {
            constexpr float kInv = 1.0f / 255.0f;
            out_rgb[0] = static_cast<float>(param.u.cd.value.red) * kInv;
            out_rgb[1] = static_cast<float>(param.u.cd.value.green) * kInv;
            out_rgb[2] = static_cast<float>(param.u.cd.value.blue) * kInv;
        }
    }
    PF_CHECKIN_PARAM(in_data, &param);
    return err;
}

}  // namespace

PF_Err SetupParams(PF_InData* in_data, PF_OutData* out_data) {
    PF_ParamDef def;

    AEFX_CLR_STRUCT(def);
    PF_ADD_TOPICX("Glow", 0, kIdGlowGroup);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Threshold", 0.0f, 4.0f, 0.0f, 1.0f, 0.5f, PF_Precision_THOUSANDTHS, 0, 0, kIdThreshold);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Threshold Softness", 0.0f, 100.0f, 0.0f, 100.0f, 40.0f, PF_Precision_TENTHS,
                         PF_ValueDisplayFlag_PERCENT, 0, kIdSoftness);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Radius", 0.0f, 4000.0f, 0.0f, 400.0f, 40.0f, PF_Precision_TENTHS, 0, 0, kIdRadius);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Intensity", 0.0f, 10000.0f, 0.0f, 400.0f, 100.0f, PF_Precision_TENTHS,
                         PF_ValueDisplayFlag_PERCENT, 0, kIdIntensity);

    AEFX_CLR_STRUCT(def);
    PF_END_TOPIC(kIdGlowGroupEnd);

    AEFX_CLR_STRUCT(def);
    PF_ADD_TOPICX("Color", 0, kIdColorGroup);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Exposure", -10.0f, 10.0f, -4.0f, 4.0f, 0.0f, PF_Precision_HUNDREDTHS, 0, 0, kIdExposure);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Saturation", 0.0f, 400.0f, 0.0f, 200.0f, 100.0f, PF_Precision_TENTHS,
                         PF_ValueDisplayFlag_PERCENT, 0, kIdSaturation);

    AEFX_CLR_STRUCT(def);
    PF_ADD_COLOR("Tint", 255, 255, 255, kIdTint);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Tint Amount", 0.0f, 100.0f, 0.0f, 100.0f, 0.0f, PF_Precision_TENTHS,
                         PF_ValueDisplayFlag_PERCENT, 0, kIdTintAmount);

    AEFX_CLR_STRUCT(def);
    PF_END_TOPIC(kIdColorGroupEnd);

    AEFX_CLR_STRUCT(def);
    PF_ADD_TOPICX("Render", 0, kIdRenderGroup);

    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUPX("Quality", 4, 2, kQualityChoices, 0, kIdQuality);

    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUPX("Composite", 3, 1, kCompositeChoices, 0, kIdComposite);

    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUPX("Working Space", 3, 1, kWorkingSpaceChoices, 0, kIdWorkingSpace);

    AEFX_CLR_STRUCT(def);
    PF_ADD_CHECKBOXX("Expand Bounds", TRUE, 0, kIdExpandBounds);

    AEFX_CLR_STRUCT(def);
    PF_END_TOPIC(kIdRenderGroupEnd);

    // An empty group whose header is the build. Nothing to configure; it is
    // there so the loaded build can be identified without leaving the panel.
    AEFX_CLR_STRUCT(def);
    PF_ADD_TOPICX("v" AB_GLOW_VERSION_STRING " (" AB_GLOW_BUILD_ID ")", 0, kIdAboutGroup);

    AEFX_CLR_STRUCT(def);
    PF_END_TOPIC(kIdAboutGroupEnd);

    out_data->num_params = kParamCount;
    return PF_Err_NONE;
}

PF_Err ReadParams(PF_InData* in_data, PF_OutData* out_data, EffectParams* out_params) {
    (void)out_data;
    PF_Err err = PF_Err_NONE;
    EffectParams params;

    float threshold = 0.5f;
    float softness = 40.0f;
    float radius = 40.0f;
    float intensity = 100.0f;
    float exposure = 0.0f;
    float saturation = 100.0f;
    float tint_amount = 0.0f;
    float tint[3] = {1.0f, 1.0f, 1.0f};
    int quality = 2;
    int composite = 1;
    int working_space = 1;
    bool expand_bounds = true;

    if (!err) err = CheckoutFloat(in_data, kParamThreshold, &threshold);
    if (!err) err = CheckoutFloat(in_data, kParamSoftness, &softness);
    if (!err) err = CheckoutFloat(in_data, kParamRadius, &radius);
    if (!err) err = CheckoutFloat(in_data, kParamIntensity, &intensity);
    if (!err) err = CheckoutFloat(in_data, kParamExposure, &exposure);
    if (!err) err = CheckoutFloat(in_data, kParamSaturation, &saturation);
    if (!err) err = CheckoutColor(in_data, kParamTint, tint);
    if (!err) err = CheckoutFloat(in_data, kParamTintAmount, &tint_amount);
    if (!err) err = CheckoutPopup(in_data, kParamQuality, &quality);
    if (!err) err = CheckoutPopup(in_data, kParamComposite, &composite);
    if (!err) err = CheckoutPopup(in_data, kParamWorkingSpace, &working_space);
    if (!err) err = CheckoutCheckbox(in_data, kParamExpandBounds, &expand_bounds);
    if (err) return err;

    // Slider distances are authored at full resolution; convert to the pixels
    // this render actually works in, and keep the glow round on non-square
    // pixels.
    const float downsample_x = RationalToFloat(in_data->downsample_x);
    const float downsample_y = RationalToFloat(in_data->downsample_y);
    const float aspect = RationalToFloat(in_data->pixel_aspect_ratio);

    GlowSettings& settings = params.settings;
    settings.threshold = std::max(0.0f, threshold);
    settings.threshold_softness = std::clamp(softness * 0.01f, 0.0f, 1.0f);
    settings.radius_x = std::max(0.0f, radius) * downsample_x / (aspect > 0.0f ? aspect : 1.0f);
    settings.radius_y = std::max(0.0f, radius) * downsample_y;
    settings.intensity = std::max(0.0f, intensity) * 0.01f;
    settings.exposure = exposure;
    settings.saturation = std::max(0.0f, saturation) * 0.01f;
    settings.tint_r = tint[0];
    settings.tint_g = tint[1];
    settings.tint_b = tint[2];
    settings.tint_amount = std::clamp(tint_amount * 0.01f, 0.0f, 1.0f);
    int quality_level = std::clamp(quality - 1, 0, 3);
    if (in_data->quality == PF_Quality_LO) quality_level = std::max(0, quality_level - 1);
    settings.quality = static_cast<Quality>(quality_level);
    settings.composite = static_cast<CompositeMode>(std::clamp(composite - 1, 0, 2));
    settings.working_space = static_cast<WorkingSpace>(std::clamp(working_space - 1, 0, 2));
    params.expand_bounds = expand_bounds;

    *out_params = params;
    return PF_Err_NONE;
}

}  // namespace abglow
