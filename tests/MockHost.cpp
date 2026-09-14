// Minimal After Effects stand-in: loads AbGlow.aex and drives the same command
// sequence the host does, so the plug-in layer can be exercised without
// launching After Effects.
//
// Usage: MockHost.exe <path to AbGlow.aex> [output directory]

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "TestSupport.h"

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunknown-pragmas"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#include "AEConfig.h"
#include "AE_Effect.h"
#include "AE_EffectCB.h"
#include "AE_EffectCBSuites.h"
#include "AE_EffectSuites.h"
#include "AE_Macros.h"
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

#include "core/Pixel.h"

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic ignored "-Wmultichar"
#endif

namespace {

using EffectMainFn = PF_Err (*)(PF_Cmd, PF_InData*, PF_OutData*, PF_ParamDef*[], PF_LayerDef*, void*);

int g_failures = 0;

void Check(bool condition, const std::string& what) {
    if (!condition) {
        ++g_failures;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

// ---------------------------------------------------------------------------
// Host state
// ---------------------------------------------------------------------------

struct MockHost {
    std::vector<PF_ParamDef> params;
    std::vector<void*> live_handles;
    int handles_created = 0;
    int handles_disposed = 0;

    PF_EffectWorld input_world = {};
    PF_EffectWorld output_world = {};
    PF_LRect layer_rect = {};
    bool input_checked_out = false;
    bool output_checked_out = false;
    int checkout_layer_calls = 0;
};

MockHost* g_host = nullptr;

// --- interaction callbacks -------------------------------------------------

PF_Err MockAddParam(PF_ProgPtr, PF_ParamIndex index, PF_ParamDefPtr def) {
    (void)index;
    g_host->params.push_back(*def);
    return PF_Err_NONE;
}

// Index 0 is the input layer, which the host owns; 1..n are the added params.
PF_Err MockCheckoutParam(PF_ProgPtr, PF_ParamIndex index, A_long, A_long, A_u_long, PF_ParamDef* param) {
    if (index <= 0 || static_cast<std::size_t>(index - 1) >= g_host->params.size()) {
        return PF_Err_BAD_CALLBACK_PARAM;
    }
    *param = g_host->params[static_cast<std::size_t>(index - 1)];
    return PF_Err_NONE;
}

PF_Err MockCheckinParam(PF_ProgPtr, PF_ParamDef*) { return PF_Err_NONE; }
PF_Err MockAbort(PF_ProgPtr) { return PF_Err_NONE; }
PF_Err MockProgress(PF_ProgPtr, A_long, A_long) { return PF_Err_NONE; }

int MockSprintf(char* buffer, const char* format, ...) {
    va_list args;
    va_start(args, format);
    const int written = vsprintf(buffer, format, args);
    va_end(args);
    return written;
}

// --- suites ----------------------------------------------------------------

PF_Handle MockNewHandle(A_HandleSize size) {
    void** block = static_cast<void**>(std::malloc(static_cast<std::size_t>(size) + sizeof(void*)));
    if (block == nullptr) return nullptr;
    block[0] = block + 1;
    ++g_host->handles_created;
    g_host->live_handles.push_back(block);
    return reinterpret_cast<PF_Handle>(block);
}

void* MockLockHandle(PF_Handle handle) {
    void** block = reinterpret_cast<void**>(handle);
    return block == nullptr ? nullptr : block[0];
}

void MockUnlockHandle(PF_Handle) {}

void MockDisposeHandle(PF_Handle handle) {
    if (handle == nullptr) return;
    void** block = reinterpret_cast<void**>(handle);
    for (std::size_t i = 0; i < g_host->live_handles.size(); ++i) {
        if (g_host->live_handles[i] == block) {
            g_host->live_handles.erase(g_host->live_handles.begin() + static_cast<std::ptrdiff_t>(i));
            ++g_host->handles_disposed;
            std::free(block);
            return;
        }
    }
    std::printf("FAIL: disposing a handle the host never created\n");
    ++g_failures;
}

A_HandleSize MockGetHandleSize(PF_Handle) { return 0; }
PF_Err MockResizeHandle(A_HandleSize, PF_Handle*) { return PF_Err_NONE; }

PF_HandleSuite1 g_handle_suite = {MockNewHandle,       MockLockHandle,    MockUnlockHandle,
                                  MockDisposeHandle,   MockGetHandleSize, MockResizeHandle};

// Runs the iterations across real threads, the way the host's pool does.
PF_Err MockIterateGeneric(A_long iterations, void* refcon,
                          PF_Err (*fn)(void*, A_long, A_long, A_long)) {
    const int hardware = static_cast<int>(std::thread::hardware_concurrency());
    const int workers = hardware > 0 ? hardware : 4;
    if (iterations == PF_Iterations_ONCE_PER_PROCESSOR) {
        for (int i = 0; i < workers; ++i) fn(refcon, i, i, workers);
        return PF_Err_NONE;
    }
    std::vector<std::thread> threads;
    const int used = workers < iterations ? workers : static_cast<int>(iterations);
    for (int t = 0; t < used; ++t) {
        threads.emplace_back([&, t]() {
            for (A_long i = t; i < iterations; i += used) fn(refcon, t, i, iterations);
        });
    }
    for (std::thread& thread : threads) thread.join();
    return PF_Err_NONE;
}

PF_Iterate8Suite1 g_iterate_suite = {};

SPErr MockAcquireSuite(const char* name, int32_t version, const void** suite) {
    if (std::strcmp(name, kPFHandleSuite) == 0 && version == kPFHandleSuiteVersion1) {
        *suite = &g_handle_suite;
        return kSPNoError;
    }
    if (std::strcmp(name, kPFIterate8Suite) == 0 && version == kPFIterate8SuiteVersion1) {
        *suite = &g_iterate_suite;
        return kSPNoError;
    }
    *suite = nullptr;
    return kSPSuiteNotFoundError;
}

SPErr MockReleaseSuite(const char*, int32_t) { return kSPNoError; }

SPBasicSuite g_basic_suite = {};

// --- smart render callbacks ------------------------------------------------

PF_Err MockCheckoutLayer(PF_ProgPtr, PF_ParamIndex index, A_long checkout_id, const PF_RenderRequest* request,
                         A_long, A_long, A_u_long, PF_CheckoutResult* result) {
    (void)index;
    (void)checkout_id;
    ++g_host->checkout_layer_calls;
    std::memset(result, 0, sizeof(*result));
    result->max_result_rect = g_host->layer_rect;
    result->ref_width = g_host->layer_rect.right - g_host->layer_rect.left;
    result->ref_height = g_host->layer_rect.bottom - g_host->layer_rect.top;
    result->par.num = 1;
    result->par.den = 1;

    PF_LRect clipped = g_host->layer_rect;
    if (request != nullptr) {
        clipped.left = request->rect.left > clipped.left ? request->rect.left : clipped.left;
        clipped.top = request->rect.top > clipped.top ? request->rect.top : clipped.top;
        clipped.right = request->rect.right < clipped.right ? request->rect.right : clipped.right;
        clipped.bottom = request->rect.bottom < clipped.bottom ? request->rect.bottom : clipped.bottom;
    }
    if (clipped.right < clipped.left) clipped.right = clipped.left;
    if (clipped.bottom < clipped.top) clipped.bottom = clipped.top;
    result->result_rect = clipped;
    return PF_Err_NONE;
}

PF_Err MockGuidMixInPtr(PF_ProgPtr, A_u_long, const void*) { return PF_Err_NONE; }

PF_Err MockCheckoutLayerPixels(PF_ProgPtr, A_long, PF_EffectWorld** pixels) {
    g_host->input_checked_out = true;
    *pixels = &g_host->input_world;
    return PF_Err_NONE;
}

PF_Err MockCheckinLayerPixels(PF_ProgPtr, A_long) { return PF_Err_NONE; }

PF_Err MockCheckoutOutput(PF_ProgPtr, PF_EffectWorld** output) {
    g_host->output_checked_out = true;
    *output = &g_host->output_world;
    return PF_Err_NONE;
}

// ---------------------------------------------------------------------------

struct WorldStorage {
    std::vector<unsigned char> bytes;
};

void MakeWorld(PF_EffectWorld* world, WorldStorage* storage, int width, int height, short bitdepth,
               const PF_LRect& rect) {
    const int bytes_per_pixel = bitdepth == 8 ? 4 : (bitdepth == 16 ? 8 : 16);
    std::memset(world, 0, sizeof(*world));
    world->width = width;
    world->height = height;
    world->rowbytes = width * bytes_per_pixel;
    storage->bytes.assign(static_cast<std::size_t>(world->rowbytes) * static_cast<std::size_t>(height), 0);
    world->data = reinterpret_cast<PF_PixelPtr>(storage->bytes.data());
    world->world_flags = bitdepth == 16 ? PF_WorldFlag_DEEP : static_cast<PF_WorldFlags>(0);
    world->origin_x = rect.left;
    world->origin_y = rect.top;
    world->extent_hint.left = 0;
    world->extent_hint.top = 0;
    world->extent_hint.right = width;
    world->extent_hint.bottom = height;
    world->pix_aspect_ratio.num = 1;
    world->pix_aspect_ratio.den = 1;
}

void FillTestScene(PF_EffectWorld* world, short bitdepth) {
    for (int y = 0; y < world->height; ++y) {
        unsigned char* row = reinterpret_cast<unsigned char*>(world->data) +
                             static_cast<std::ptrdiff_t>(y) * world->rowbytes;
        for (int x = 0; x < world->width; ++x) {
            const bool bright = (x > world->width / 4 && x < world->width / 2 && y > world->height / 3 &&
                                 y < 2 * world->height / 3);
            const bool dim = (x > world->width / 2 + 20 && x < world->width - 20 && y > 20 && y < 60);
            float value = bright ? 1.0f : (dim ? 0.25f : 0.0f);
            float alpha = (bright || dim) ? 1.0f : 0.0f;
            if (bitdepth == 8) {
                abglow::Pixel8& p = reinterpret_cast<abglow::Pixel8*>(row)[x];
                p.a = static_cast<unsigned char>(alpha * 255.0f);
                p.r = p.g = p.b = static_cast<unsigned char>(value * 255.0f);
            } else if (bitdepth == 16) {
                abglow::Pixel16& p = reinterpret_cast<abglow::Pixel16*>(row)[x];
                p.a = static_cast<unsigned short>(alpha * abglow::kMaxChannel16);
                p.r = p.g = p.b = static_cast<unsigned short>(value * abglow::kMaxChannel16);
            } else {
                abglow::PixelF& p = reinterpret_cast<abglow::PixelF*>(row)[x];
                p.a = alpha;
                // HDR core, so the 32 bpc path gets values above 1.0.
                p.r = p.g = p.b = bright ? 12.0f : value;
            }
        }
    }
}

void SetupInData(PF_InData* in_data, PF_OutData* out_data, PF_UtilCallbacks* utils, MockHost* host) {
    std::memset(in_data, 0, sizeof(*in_data));
    std::memset(out_data, 0, sizeof(*out_data));
    std::memset(utils, 0, sizeof(*utils));

    utils->ansi.sprintf = MockSprintf;

    in_data->inter.add_param = MockAddParam;
    in_data->inter.checkout_param = MockCheckoutParam;
    in_data->inter.checkin_param = MockCheckinParam;
    in_data->inter.abort = MockAbort;
    in_data->inter.progress = MockProgress;
    in_data->utils = reinterpret_cast<struct _PF_UtilCallbacks*>(utils);
    in_data->effect_ref = reinterpret_cast<PF_ProgPtr>(host);
    in_data->pica_basicP = &g_basic_suite;
    in_data->quality = PF_Quality_HI;
    in_data->version.major = PF_AE_PLUG_IN_VERSION;
    in_data->version.minor = PF_AE_PLUG_IN_SUBVERS;
    in_data->time_scale = 30;
    in_data->time_step = 1;
    in_data->downsample_x.num = 1;
    in_data->downsample_x.den = 1;
    in_data->downsample_y.num = 1;
    in_data->downsample_y.den = 1;
    in_data->pixel_aspect_ratio.num = 1;
    in_data->pixel_aspect_ratio.den = 1;
}

void SetFloatParam(MockHost* host, int index, float value) {
    host->params[static_cast<std::size_t>(index)].u.fs_d.value = value;
}

void SetPopupParam(MockHost* host, int index, int value) {
    host->params[static_cast<std::size_t>(index)].u.pd.value = value;
}

void SetCheckboxParam(MockHost* host, int index, bool value) {
    host->params[static_cast<std::size_t>(index)].u.bd.value = value ? TRUE : FALSE;
}

struct RenderOptions {
    short bitdepth = 8;
    int width = 480;
    int height = 270;
    float radius = 60.0f;
    float threshold = 0.5f;
    bool expand_bounds = true;
    int quality = 2;  // 1 based popup value
    std::string label;
    bool advanced = false;  // exercise the appended Advanced group
};

// Indices must match ParamIndex in AbGlowParams.h.
enum {
    kIndexThreshold = 2,
    kIndexSoftness = 3,
    kIndexRadius = 4,
    kIndexIntensity = 5,
    kIndexExposure = 8,
    kIndexSaturation = 9,
    kIndexTintAmount = 11,
    kIndexQuality = 14,
    kIndexComposite = 15,
    kIndexWorkingSpace = 16,
    kIndexExpandBounds = 17,
    kIndexSaturationBias = 24,
    kIndexSourceOpacity = 25,
    kIndexUnmult = 26,
    kIndexMultiplyRed = 27,
    kIndexMultiplyGreen = 28,
    kIndexMultiplyBlue = 29,
    kParamCountAsShipped = 31
};

bool RunRender(EffectMainFn effect_main, const RenderOptions& options, const std::string& out_dir) {
    MockHost host;
    g_host = &host;

    PF_InData in_data;
    PF_OutData out_data;
    PF_UtilCallbacks utils;
    SetupInData(&in_data, &out_data, &utils, &host);
    in_data.width = options.width;
    in_data.height = options.height;

    PF_Err err = effect_main(PF_Cmd_GLOBAL_SETUP, &in_data, &out_data, nullptr, nullptr, nullptr);
    Check(err == PF_Err_NONE, "global setup succeeds");

    err = effect_main(PF_Cmd_PARAMS_SETUP, &in_data, &out_data, nullptr, nullptr, nullptr);
    Check(err == PF_Err_NONE, "params setup succeeds");
    Check(out_data.num_params == static_cast<A_long>(host.params.size()) + 1,
          "reported parameter count matches the parameters added");
    // Saved projects address parameters by position, so the count is part of
    // the file format: it may grow, never shrink or shuffle.
    Check(out_data.num_params == kParamCountAsShipped, "the shipped parameter layout is unchanged");

    SetFloatParam(&host, kIndexThreshold - 1, options.threshold);
    SetFloatParam(&host, kIndexSoftness - 1, 40.0f);
    SetFloatParam(&host, kIndexRadius - 1, options.radius);
    SetFloatParam(&host, kIndexIntensity - 1, 150.0f);
    SetFloatParam(&host, kIndexExposure - 1, 0.5f);
    SetFloatParam(&host, kIndexSaturation - 1, 120.0f);
    SetFloatParam(&host, kIndexTintAmount - 1, 0.0f);
    SetPopupParam(&host, kIndexQuality - 1, options.quality);
    SetPopupParam(&host, kIndexComposite - 1, 1);
    SetPopupParam(&host, kIndexWorkingSpace - 1, 1);
    SetCheckboxParam(&host, kIndexExpandBounds - 1, options.expand_bounds);
    if (options.advanced) {
        SetFloatParam(&host, kIndexSaturationBias - 1, 60.0f);
        SetFloatParam(&host, kIndexSourceOpacity - 1, 70.0f);
        SetCheckboxParam(&host, kIndexUnmult - 1, true);
        SetFloatParam(&host, kIndexMultiplyRed - 1, 140.0f);
        SetFloatParam(&host, kIndexMultiplyGreen - 1, 100.0f);
        SetFloatParam(&host, kIndexMultiplyBlue - 1, 70.0f);
    }

    host.layer_rect.left = 0;
    host.layer_rect.top = 0;
    host.layer_rect.right = options.width;
    host.layer_rect.bottom = options.height;

    PF_PreRenderInput pre_input = {};
    pre_input.bitdepth = options.bitdepth;
    pre_input.output_request.rect = host.layer_rect;
    pre_input.output_request.field = PF_Field_FRAME;
    pre_input.output_request.channel_mask = PF_ChannelMask_ARGB;

    PF_PreRenderOutput pre_output = {};
    PF_PreRenderCallbacks pre_callbacks = {MockCheckoutLayer, MockGuidMixInPtr};
    PF_PreRenderExtra pre_extra = {&pre_input, &pre_output, &pre_callbacks};

    err = effect_main(PF_Cmd_SMART_PRE_RENDER, &in_data, &out_data, nullptr, nullptr, &pre_extra);
    Check(err == PF_Err_NONE, "smart pre-render succeeds");
    Check(pre_output.pre_render_data != nullptr, "pre-render data was allocated");

    const A_long result_width = pre_output.result_rect.right - pre_output.result_rect.left;
    const A_long result_height = pre_output.result_rect.bottom - pre_output.result_rect.top;
    if (options.expand_bounds) {
        Check(result_width > options.width && result_height > options.height,
              "expanded bounds grow the result rect");
        Check((pre_output.flags & PF_RenderOutputFlag_RETURNS_EXTRA_PIXELS) != 0,
              "extra pixels flag is set when the result grows");
    } else {
        Check(result_width == options.width && result_height == options.height,
              "result rect matches the layer when bounds are not expanded");
    }
    Check(pre_output.max_result_rect.right - pre_output.max_result_rect.left == result_width,
          "max result rect matches the result rect");

    WorldStorage input_storage;
    WorldStorage output_storage;
    MakeWorld(&host.input_world, &input_storage, options.width, options.height, options.bitdepth,
              host.layer_rect);
    MakeWorld(&host.output_world, &output_storage, static_cast<int>(result_width),
              static_cast<int>(result_height), options.bitdepth, pre_output.result_rect);
    FillTestScene(&host.input_world, options.bitdepth);

    PF_SmartRenderInput render_input = {};
    render_input.output_request = pre_input.output_request;
    render_input.bitdepth = options.bitdepth;
    render_input.pre_render_data = pre_output.pre_render_data;

    PF_SmartRenderCallbacks render_callbacks = {MockCheckoutLayerPixels, MockCheckinLayerPixels,
                                                MockCheckoutOutput};
    PF_SmartRenderExtra render_extra = {&render_input, &render_callbacks};

    err = effect_main(PF_Cmd_SMART_RENDER, &in_data, &out_data, nullptr, nullptr, &render_extra);
    Check(err == PF_Err_NONE, "smart render succeeds");
    Check(host.input_checked_out && host.output_checked_out, "render checked out its buffers");
    Check(host.handles_created == host.handles_disposed, "every scratch handle was released");
    Check(host.live_handles.empty(), "no handles leaked");

    if (pre_output.delete_pre_render_data_func != nullptr) {
        pre_output.delete_pre_render_data_func(pre_output.pre_render_data);
    }

    // The source must still be visible in the middle of the expanded frame.
    abglow_test::TestImage image(static_cast<int>(result_width), static_cast<int>(result_height),
                                 options.bitdepth == 8 ? abglow::PixelDepth::kBits8
                                                       : (options.bitdepth == 16 ? abglow::PixelDepth::kBits16
                                                                                 : abglow::PixelDepth::kFloat32));
    std::memcpy(image.View().data, output_storage.bytes.data(), output_storage.bytes.size());

    const int centre_x = static_cast<int>(result_width) / 2;
    const int centre_y = static_cast<int>(result_height) / 2;
    const abglow::PixelF centre = image.GetPixel(centre_x, centre_y);
    Check(centre.r > 0.0f, "the rendered frame is not empty");

    if (!out_dir.empty()) {
        abglow_test::WritePng(out_dir + "/mockhost_" + options.label + ".png", image);
    }

    g_host = nullptr;
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: MockHost <path to AbGlow.aex> [output directory]\n");
        return 2;
    }
    const std::string plugin_path = argv[1];
    const std::string out_dir = argc > 2 ? argv[2] : "";

    g_basic_suite.AcquireSuite = MockAcquireSuite;
    g_basic_suite.ReleaseSuite = MockReleaseSuite;
    g_iterate_suite.iterate_generic = MockIterateGeneric;

    HMODULE module = LoadLibraryA(plugin_path.c_str());
    if (module == nullptr) {
        std::printf("FAIL: could not load %s (error %lu)\n", plugin_path.c_str(), GetLastError());
        return 1;
    }

    EffectMainFn effect_main = reinterpret_cast<EffectMainFn>(GetProcAddress(module, "EffectMain"));
    Check(effect_main != nullptr, "EffectMain is exported");
    Check(GetProcAddress(module, "PluginDataEntryFunction2") != nullptr,
          "PluginDataEntryFunction2 is exported");
    if (effect_main == nullptr) return 1;

    const std::vector<RenderOptions> cases = {
        {8, 480, 270, 60.0f, 0.5f, true, 2, "8bpc"},
        {16, 480, 270, 60.0f, 0.5f, true, 2, "16bpc"},
        {32, 480, 270, 60.0f, 0.5f, true, 2, "32bpc"},
        {32, 480, 270, 4.0f, 0.5f, true, 2, "small_radius"},
        {32, 480, 270, 400.0f, 0.5f, true, 4, "large_radius"},
        {8, 480, 270, 60.0f, 0.5f, false, 1, "no_expand"},
        {8, 97, 61, 30.0f, 0.2f, true, 3, "odd_size"},
        {32, 480, 270, 60.0f, 0.5f, true, 2, "advanced", true},
    };

    for (const RenderOptions& options : cases) {
        std::printf("-- %s (%d bpc, %dx%d, radius %.0f)\n", options.label.c_str(), options.bitdepth,
                    options.width, options.height, options.radius);
        RunRender(effect_main, options, out_dir);
    }

    // Applying and removing the effect repeatedly must not leak or crash.
    for (int i = 0; i < 25; ++i) {
        RenderOptions options = cases[0];
        options.label = "repeat";
        RunRender(effect_main, options, "");
    }

    FreeLibrary(module);
    std::printf("%s\n", g_failures == 0 ? "mock host: all checks passed" : "mock host: FAILURES");
    return g_failures == 0 ? 0 : 1;
}
