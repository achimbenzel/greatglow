#include "AbGlowRender.h"

#include "Diagnostics.h"

#include <algorithm>
#include <cmath>
#include <new>

#include "AbGlowParams.h"
#include "AeAdapters.h"
#include "core/GlowPipeline.h"
#include "core/GlowPlan.h"

namespace abglow {
namespace {

constexpr A_long kProbeCheckoutId = 1;
constexpr A_long kInputCheckoutId = 2;

// Guard rail so an extreme radius cannot ask After Effects for an absurd buffer.
// A large radius wants a lot of room: at radius 1240 the glow reaches 2542 px,
// and clipping that cut a fifth off its extent. The buffer this allows is big
// but still one After Effects will hand over.
constexpr A_long kMaxBoundsExpansion = 3000;

struct PreRenderData {
    EffectParams params;
    PF_LRect output_rect = {};
    PF_LRect input_rect = {};
    bool has_input = false;
};

void DeletePreRenderData(void* data) {
    delete static_cast<PreRenderData*>(data);
}

bool IsEmpty(const PF_LRect& rect) {
    return rect.right <= rect.left || rect.bottom <= rect.top;
}

PF_LRect Inflate(const PF_LRect& rect, A_long amount) {
    PF_LRect out = rect;
    out.left -= amount;
    out.top -= amount;
    out.right += amount;
    out.bottom += amount;
    return out;
}

// `layer_width`/`layer_height` are the layer's size in render pixels. The plan
// depends on them, so pre-render and render must be given the same ones or the
// bounds would describe a glow the render does not produce.
A_long BoundsExpansion(const GlowSettings& settings, bool expand_bounds, A_long layer_width,
                       A_long layer_height) {
    if (!expand_bounds) return 0;
    if (settings.intensity <= 0.0f) return 0;
    const float sigma = RadiusToSigma(std::max(settings.radius_x, settings.radius_y));
    const GlowPlan plan = MakeGlowPlan(sigma, settings.quality, static_cast<int>(layer_width),
                                       static_cast<int>(layer_height));
    const float reach = plan.Reach();
    if (!(reach > 0.0f)) return 0;
    return std::min<A_long>(kMaxBoundsExpansion, static_cast<A_long>(std::ceil(reach)));
}

// Where the world's top-left sits in layer coordinates. When the host hands
// back exactly the rectangle we asked for, that rectangle is authoritative.
void ResolveWorldOrigin(const PF_EffectWorld* world, const PF_LRect& declared, A_long* left, A_long* top) {
    const A_long width = declared.right - declared.left;
    const A_long height = declared.bottom - declared.top;
    if (world->width == width && world->height == height) {
        *left = declared.left;
        *top = declared.top;
    } else {
        *left = world->origin_x;
        *top = world->origin_y;
    }
}

PF_Err RunPipeline(PF_InData* in_data, const GlowSettings& settings, PF_EffectWorld* input_world,
                   PF_EffectWorld* output_world, int offset_x, int offset_y, PixelDepth depth) {
    SPBasicSuite* basic = in_data->pica_basicP;
    if (basic == nullptr) return PF_Err_BAD_CALLBACK_PARAM;

    PF_HandleSuite1* handle_suite = AcquireSuite<PF_HandleSuite1>(basic, kPFHandleSuite, kPFHandleSuiteVersion1);
    if (handle_suite == nullptr) return PF_Err_OUT_OF_MEMORY;
    PF_Iterate8Suite1* iterate_suite =
        AcquireSuite<PF_Iterate8Suite1>(basic, kPFIterate8Suite, kPFIterate8SuiteVersion1);

    PF_Err err = PF_Err_NONE;
    {
        AeAllocator allocator(in_data, handle_suite);
        AeTaskRunner runner(in_data, iterate_suite);

        GlowRender render;
        render.source = MakeHostImage(input_world, depth);
        render.dest = MakeHostImage(output_world, depth);
        render.source_offset_x = offset_x;
        render.source_offset_y = offset_y;

        if (diag::Enabled()) {
            const GlowPlan plan = PlanForRender(settings, render);
            diag::Log("        source=%dx%d dest=%dx%d  plan: scale=%d levels=%d level_sigma=%.3f "
                      "eff_sigma=%.1f reach=%.1f",
                      render.source.width, render.source.height, render.dest.width, render.dest.height,
                      plan.base_scale, plan.level_count, plan.level_sigma, plan.EffectiveSigma(), plan.Reach());
        }

        switch (RenderGlow(settings, render, allocator, runner)) {
            case GlowResult::kOk: break;
            case GlowResult::kOutOfMemory: err = PF_Err_OUT_OF_MEMORY; break;
            case GlowResult::kInvalidArguments: err = PF_Err_BAD_CALLBACK_PARAM; break;
        }
    }

    if (iterate_suite != nullptr) basic->ReleaseSuite(kPFIterate8Suite, kPFIterate8SuiteVersion1);
    basic->ReleaseSuite(kPFHandleSuite, kPFHandleSuiteVersion1);
    return err;
}

}  // namespace

PF_Err SmartPreRender(PF_InData* in_data, PF_OutData* out_data, PF_PreRenderExtra* extra) {
    PF_Err err = PF_Err_NONE;

    EffectParams params;
    ERR(ReadParams(in_data, out_data, &params));
    if (err) return err;

    PF_RenderRequest request = extra->input->output_request;
    request.channel_mask = PF_ChannelMask_ARGB;

    // An empty request is the cheap way to learn the layer's real extent.
    PF_RenderRequest probe = request;
    probe.rect.left = 0;
    probe.rect.top = 0;
    probe.rect.right = 0;
    probe.rect.bottom = 0;

    PF_CheckoutResult probe_result = {};
    ERR(extra->cb->checkout_layer(in_data->effect_ref, kParamInput, kProbeCheckoutId, &probe,
                                  in_data->current_time, in_data->time_step, in_data->time_scale, &probe_result));
    if (err) return err;

    const PF_LRect layer_rect = probe_result.max_result_rect;
    if (IsEmpty(layer_rect)) {
        extra->output->result_rect = layer_rect;
        extra->output->max_result_rect = layer_rect;
        extra->output->solid = FALSE;
        return PF_Err_NONE;
    }

    // Plan against the layer's own rect, not in_data's nominal size: that is
    // what the render is handed, and the plan depends on it, so using anything
    // else could declare bounds for a glow the render does not produce.
    const A_long expansion = BoundsExpansion(params.settings, params.expand_bounds,
                                             layer_rect.right - layer_rect.left,
                                             layer_rect.bottom - layer_rect.top);
    const PF_LRect output_rect = Inflate(layer_rect, expansion);

    diag::Frame();
    diag::Log("prerender layer=%dx%d downsample=%d/%d,%d/%d quality=%d depth=%d", (int)in_data->width,
              (int)in_data->height, (int)in_data->downsample_x.num, (int)in_data->downsample_x.den,
              (int)in_data->downsample_y.num, (int)in_data->downsample_y.den, (int)in_data->quality,
              (int)extra->input->bitdepth);
    diag::Log("  radius=%.1f,%.1f intensity=%.3f exposure=%.2f threshold=%.3f q=%d expand=%d",
              params.settings.radius_x, params.settings.radius_y, params.settings.intensity,
              params.settings.exposure, params.settings.threshold, (int)params.settings.quality,
              params.expand_bounds ? 1 : 0);
    diag::Log("  falloff=%.2f multiply=%.2f,%.2f,%.2f satbias=%.2f srcopacity=%.2f unmult=%d rolloff=%d",
              params.settings.falloff, params.settings.aberration_r, params.settings.aberration_g,
              params.settings.aberration_b, params.settings.saturation_bias, params.settings.source_opacity,
              params.settings.unmult ? 1 : 0, (int)params.settings.rolloff);
    diag::Log("  request rect  = [%d %d %d %d]", (int)request.rect.left, (int)request.rect.top,
              (int)request.rect.right, (int)request.rect.bottom);
    diag::Log("  layer  rect   = [%d %d %d %d]", (int)layer_rect.left, (int)layer_rect.top, (int)layer_rect.right,
              (int)layer_rect.bottom);
    diag::Log("  expansion=%d declared=[%d %d %d %d]", (int)expansion, (int)output_rect.left,
              (int)output_rect.top, (int)output_rect.right, (int)output_rect.bottom);

    // The glow needs the whole layer, so ask for all of it and always render the
    // full result. After Effects caches the frame, so partial requests would
    // only re-do the same work.
    PF_RenderRequest full = request;
    full.rect = layer_rect;

    PF_CheckoutResult input_result = {};
    ERR(extra->cb->checkout_layer(in_data->effect_ref, kParamInput, kInputCheckoutId, &full, in_data->current_time,
                                  in_data->time_step, in_data->time_scale, &input_result));
    if (err) return err;

    PreRenderData* data = new (std::nothrow) PreRenderData();
    if (data == nullptr) return PF_Err_OUT_OF_MEMORY;
    data->params = params;
    data->output_rect = output_rect;
    data->input_rect = input_result.result_rect;
    data->has_input = !IsEmpty(input_result.result_rect);

    diag::Log("  input granted = [%d %d %d %d] max=[%d %d %d %d]", (int)input_result.result_rect.left,
              (int)input_result.result_rect.top, (int)input_result.result_rect.right,
              (int)input_result.result_rect.bottom, (int)input_result.max_result_rect.left,
              (int)input_result.max_result_rect.top, (int)input_result.max_result_rect.right,
              (int)input_result.max_result_rect.bottom);

    extra->output->result_rect = output_rect;
    extra->output->max_result_rect = output_rect;
    extra->output->solid = FALSE;
    extra->output->flags |= PF_RenderOutputFlag_RETURNS_EXTRA_PIXELS;
    extra->output->pre_render_data = data;
    extra->output->delete_pre_render_data_func = DeletePreRenderData;
    return PF_Err_NONE;
}

PF_Err SmartRender(PF_InData* in_data, PF_OutData* out_data, PF_SmartRenderExtra* extra) {
    (void)out_data;
    PF_Err err = PF_Err_NONE;

    PreRenderData* data = static_cast<PreRenderData*>(extra->input->pre_render_data);
    if (data == nullptr) return PF_Err_INTERNAL_STRUCT_DAMAGED;

    PF_EffectWorld* input_world = nullptr;
    PF_EffectWorld* output_world = nullptr;

    if (data->has_input) {
        ERR(extra->cb->checkout_layer_pixels(in_data->effect_ref, kInputCheckoutId, &input_world));
    }
    ERR(extra->cb->checkout_output(in_data->effect_ref, &output_world));
    if (err) return err;
    if (output_world == nullptr) return PF_Err_NONE;

    A_long output_left = data->output_rect.left;
    A_long output_top = data->output_rect.top;
    ResolveWorldOrigin(output_world, data->output_rect, &output_left, &output_top);

    A_long offset_x = 0;
    A_long offset_y = 0;
    if (input_world != nullptr && input_world->data != nullptr) {
        A_long input_left = data->input_rect.left;
        A_long input_top = data->input_rect.top;
        ResolveWorldOrigin(input_world, data->input_rect, &input_left, &input_top);
        offset_x = output_left - input_left;
        offset_y = output_top - input_top;
    } else {
        input_world = nullptr;
    }

    const PixelDepth depth = DepthFromBitsPerChannel(extra->input->bitdepth);

    diag::Log("render  output world=%dx%d origin=(%d,%d) rowbytes=%d -> resolved (%d,%d)",
              (int)output_world->width, (int)output_world->height, (int)output_world->origin_x,
              (int)output_world->origin_y, (int)output_world->rowbytes, (int)output_left, (int)output_top);
    if (input_world != nullptr) {
        diag::Log("        input  world=%dx%d origin=(%d,%d) rowbytes=%d", (int)input_world->width,
                  (int)input_world->height, (int)input_world->origin_x, (int)input_world->origin_y,
                  (int)input_world->rowbytes);
    } else {
        diag::Log("        input  world=none");
    }
    diag::Log("        offset=(%d,%d) depth=%d", (int)offset_x, (int)offset_y, (int)depth);

    return RunPipeline(in_data, data->params.settings, input_world, output_world, static_cast<int>(offset_x),
                       static_cast<int>(offset_y), depth);
}

PF_Err LegacyFrameSetup(PF_InData* in_data, PF_OutData* out_data) {
    PF_Err err = PF_Err_NONE;

    EffectParams params;
    ERR(ReadParams(in_data, out_data, &params));
    if (err) return err;

    const A_long expansion = BoundsExpansion(params.settings, params.expand_bounds, in_data->width, in_data->height);
    out_data->width = in_data->width + 2 * expansion;
    out_data->height = in_data->height + 2 * expansion;
    out_data->origin.h = static_cast<A_short>(expansion);
    out_data->origin.v = static_cast<A_short>(expansion);
    return PF_Err_NONE;
}

PF_Err LegacyRender(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[], PF_LayerDef* output) {
    PF_Err err = PF_Err_NONE;

    EffectParams effect_params;
    ERR(ReadParams(in_data, out_data, &effect_params));
    if (err) return err;

    PF_EffectWorld* input_world = &params[kParamInput]->u.ld;
    const PixelDepth depth = PF_WORLD_IS_DEEP(output) ? PixelDepth::kBits16 : PixelDepth::kBits8;

    // output_origin says where the input sits inside a buffer we expanded.
    return RunPipeline(in_data, effect_params.settings, input_world, output,
                       -static_cast<int>(in_data->output_origin_x), -static_cast<int>(in_data->output_origin_y),
                       depth);
}

}  // namespace abglow
