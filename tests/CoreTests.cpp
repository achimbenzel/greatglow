#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "TestSupport.h"
#include "core/Blur.h"
#include "core/GlowPipeline.h"
#include "core/GlowPlan.h"
#include "core/Transfer.h"

namespace {

int g_failures = 0;
int g_checks = 0;

void Check(bool condition, const std::string& what) {
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

void CheckNear(float actual, float expected, float tolerance, const std::string& what) {
    ++g_checks;
    if (!(std::fabs(actual - expected) <= tolerance)) {
        ++g_failures;
        std::printf("FAIL: %s (got %g, expected %g +-%g)\n", what.c_str(), actual, expected, tolerance);
    }
}

using abglow::AlphaMode;
using abglow::CompositeMode;
using abglow::GlowModel;
using abglow::GlowRender;
using abglow::GlowResult;
using abglow::GlowSettings;
using abglow::PixelDepth;
using abglow::PixelF;
using abglow::Quality;
using abglow::WorkingSpace;
using abglow_test::MallocAllocator;
using abglow_test::TestImage;
using abglow_test::ThreadPoolRunner;

GlowSettings DefaultSettings() {
    GlowSettings s;
    s.threshold = 0.3f;
    s.threshold_softness = 0.4f;
    s.radius_x = 24.0f;
    s.radius_y = 24.0f;
    s.intensity = 1.0f;
    s.working_space = WorkingSpace::kLinear;
    s.dither = false;
    return s;
}

const char* ModelName(GlowModel model) {
    return model == GlowModel::kClassic ? " (classic)" : " (inverse square)";
}

float TotalEnergy(const TestImage& image) {
    float sum = 0.0f;
    for (int y = 0; y < image.View().height; ++y) {
        for (int x = 0; x < image.View().width; ++x) {
            const PixelF p = image.GetPixel(x, y);
            sum += p.r + p.g + p.b;
        }
    }
    return sum;
}

void TestPlanSanity() {
    const int layer_w = 1920;
    const int layer_h = 1080;
    for (float radius : {0.5f, 2.0f, 8.0f, 40.0f, 200.0f, 1000.0f}) {
        const float sigma = abglow::RadiusToSigma(radius);
        const abglow::GlowPlan plan = abglow::MakeGlowPlan(sigma, Quality::kNormal, layer_w, layer_h);
        float sum = 0.0f;
        for (int i = 0; i < plan.level_count; ++i) sum += plan.weights[i];
        CheckNear(sum, 1.0f, 1e-4f, "plan weights normalise at radius " + std::to_string(radius));
        Check(plan.level_count >= 1 && plan.level_count <= abglow::kMaxPyramidLevels, "plan level count in range");
        Check(plan.base_scale >= 1 && plan.base_scale <= abglow::kMaxBaseScale, "plan base scale in range");
    }

    float previous = 0.0f;
    for (float radius = 1.0f; radius <= 400.0f; radius *= 1.3f) {
        const abglow::GlowPlan plan = abglow::MakeGlowPlan(abglow::RadiusToSigma(radius), Quality::kNormal, layer_w, layer_h);
        const float sigma = plan.EffectiveSigma();
        // Below about five pixels the ladder is still shortening, and dropping
        // an octave renormalises the rest; the step is under a percent.
        Check(sigma > previous * 0.99f, "effective sigma grows with radius");
        previous = sigma;
    }
}

void TestGaussianKernelNormalised() {
    for (float sigma : {0.5f, 1.0f, 2.5f, 6.0f}) {
        const abglow::BlurKernel kernel = abglow::BlurKernel::Gaussian(sigma);
        float sum = kernel.weights[0];
        for (int t = 1; t <= kernel.radius; ++t) sum += 2.0f * kernel.weights[t];
        CheckNear(sum, 1.0f, 1e-5f, "gaussian kernel sums to one");
    }
}

void TestTransferRoundTrip() {
    const abglow::TransferFunction& srgb = abglow::TransferFunction::Srgb();
    for (float v = 0.0f; v <= 1.0f; v += 0.01f) {
        const float round_trip = srgb.Encode(srgb.Decode(v));
        CheckNear(round_trip, v, 1.5e-4f, "sRGB round trip");
    }
    CheckNear(srgb.Decode(1.0f), 1.0f, 1e-5f, "sRGB white decodes to one");
    CheckNear(srgb.Decode(0.5f), 0.2140f, 2e-3f, "sRGB mid grey decodes to 0.214");
    Check(srgb.Decode(4.0f) > 4.0f, "sRGB decode extends above one");
}

// A lone bright pixel must spread without gaining or losing light.
void TestEnergyConservation() {
    MallocAllocator allocator;
    ThreadPoolRunner runner(4);

    TestImage source(128, 128, PixelDepth::kFloat32);
    TestImage dest(128, 128, PixelDepth::kFloat32);
    source.SetPixel(64, 64, PixelF{1.0f, 8.0f, 8.0f, 8.0f});

    GlowSettings settings = DefaultSettings();
    settings.threshold = 0.0f;
    settings.radius_x = settings.radius_y = 16.0f;
    settings.composite = CompositeMode::kGlowOnly;

    GlowRender render;
    render.source = source.View();
    render.dest = dest.View();

    Check(abglow::RenderGlow(settings, render, allocator, runner) == GlowResult::kOk, "render succeeds");
    const float energy = TotalEnergy(dest);
    CheckNear(energy, 24.0f, 24.0f * 0.05f, "glow preserves total light energy");
    Check(allocator.allocations() == allocator.frees(), "no leaked scratch buffers");
}

// The radial falloff must decay smoothly: no rings, no plateaus, no reversals.
void TestRadialFalloff() {
    MallocAllocator allocator;
    ThreadPoolRunner runner(4);

    const int size = 512;
    TestImage source(size, size, PixelDepth::kFloat32);
    TestImage dest(size, size, PixelDepth::kFloat32);
    for (int y = 250; y < 262; ++y) {
        for (int x = 250; x < 262; ++x) source.SetPixel(x, y, PixelF{1.0f, 4.0f, 4.0f, 4.0f});
    }

    GlowSettings settings = DefaultSettings();
    settings.threshold = 0.0f;
    settings.radius_x = settings.radius_y = 60.0f;
    settings.composite = CompositeMode::kGlowOnly;
    settings.quality = Quality::kHigh;

    GlowRender render;
    render.source = source.View();
    render.dest = dest.View();
    Check(abglow::RenderGlow(settings, render, allocator, runner) == GlowResult::kOk, "falloff render succeeds");

    std::vector<float> profile;
    for (int x = 256; x < size; ++x) profile.push_back(dest.GetPixel(x, 256).g);

    bool monotonic = true;
    for (std::size_t i = 1; i < profile.size(); ++i) {
        if (profile[i] > profile[i - 1] + 1e-6f) monotonic = false;
    }
    Check(monotonic, "radial profile decreases monotonically");
    Check(profile[0] > profile[40] * 2.0f, "glow has a concentrated core");
    Check(profile[72] > 0.0f, "glow keeps a soft tail out to the radius");
}

// A bilinear upsample out of the pyramid leaves a kink every base_scale pixels,
// which reads as concentric rings in a wide faint glow. Curvature must be
// spread across the cell, not concentrated at its edges.
void TestNoUpsampleCreases() {
    MallocAllocator allocator;
    ThreadPoolRunner runner(4);

    const int width = 1600;
    const int height = 300;
    TestImage source(width, height, PixelDepth::kFloat32);
    TestImage dest(width, height, PixelDepth::kFloat32);
    for (int y = height / 2 - 20; y < height / 2 + 20; ++y) {
        for (int x = width / 2 - 60; x < width / 2 + 60; ++x) {
            source.SetPixel(x, y, PixelF{1.0f, 3.0f, 3.0f, 3.0f});
        }
    }

    GlowSettings settings = DefaultSettings();
    settings.threshold = 0.0f;
    settings.radius_x = settings.radius_y = 250.0f;
    settings.composite = CompositeMode::kGlowOnly;

    for (Quality quality : {Quality::kDraft, Quality::kNormal, Quality::kHigh, Quality::kBest}) {
        settings.quality = quality;
        GlowRender render;
        render.source = source.View();
        render.dest = dest.View();
        Check(abglow::RenderGlow(settings, render, allocator, runner) == GlowResult::kOk,
              "crease render succeeds");

        const abglow::GlowPlan plan =
            abglow::MakeGlowPlan(abglow::RadiusToSigma(settings.radius_x), quality, width, height,
                                 abglow::MinimumBaseScale(width, height, quality));
        const int cell = plan.base_scale;
        if (cell < 2) continue;

        std::vector<double> curvature(static_cast<std::size_t>(cell), 0.0);
        for (int x = width / 2 + 80; x < width - 2; ++x) {
            const double a = dest.GetPixel(x - 1, height / 2).g;
            const double b = dest.GetPixel(x, height / 2).g;
            const double c = dest.GetPixel(x + 1, height / 2).g;
            const double value = b > 1e-9 ? b : 1e-9;
            curvature[static_cast<std::size_t>(x % cell)] += std::fabs(a - 2.0 * b + c) / value;
        }
        double lowest = curvature[0];
        double highest = curvature[0];
        for (double entry : curvature) {
            lowest = std::min(lowest, entry);
            highest = std::max(highest, entry);
        }
        // Bilinear gives ratios in the thousands; a smooth filter stays near 1.
        Check(highest <= lowest * 5.0, "upsample leaves no creases at quality " +
                                           std::to_string(static_cast<int>(quality)));
    }
}

// Width measured left-to-right, so a sub-pixel shift of the glow cancels out.
float GlowWidth(const TestImage& image, int row, float level) {
    float peak = 0.0f;
    for (int x = 0; x < image.View().width; ++x) peak = std::max(peak, image.GetPixel(x, row).g);
    if (peak <= 0.0f) return -1.0f;
    float left = -1.0f;
    float right = -1.0f;
    for (int x = 1; x < image.View().width; ++x) {
        const float a = image.GetPixel(x - 1, row).g / peak;
        const float b = image.GetPixel(x, row).g / peak;
        if (left < 0.0f && a < level && b >= level) left = static_cast<float>(x - 1) + (level - a) / (b - a);
        if (left >= 0.0f && a >= level && b < level) right = static_cast<float>(x - 1) + (a - level) / (a - b);
    }
    return (left < 0.0f || right < 0.0f) ? -1.0f : right - left;
}

float GlowCentroid(const TestImage& image, int row) {
    double mass = 0.0;
    double moment = 0.0;
    for (int x = 0; x < image.View().width; ++x) {
        const double v = image.GetPixel(x, row).g;
        mass += v;
        moment += v * x;
    }
    return mass > 0.0 ? static_cast<float>(moment / mass) : -1.0f;
}

// The glow must not slide sideways as the radius animates. Two things used to
// move it: the pyramid grid was anchored to the expanded destination rectangle,
// whose corner moves with the radius, and the partially covered blocks at the
// layer's far edge were dropped, which shifted the layer's centre of mass
// whenever the radius changed the pyramid step. The layer size here is
// deliberately not a multiple of any likely step.
void TestGlowDoesNotSlideWithRadius(GlowModel model) {
    MallocAllocator allocator;
    ThreadPoolRunner runner(4);

    const int layer_w = 233;
    const int layer_h = 79;
    float previous = -1.0f;
    float worst = 0.0f;

    for (float radius = 200.0f; radius <= 240.0f; radius += 4.0f) {
        GlowSettings settings = DefaultSettings();
        settings.model = model;
        settings.threshold = 0.0f;
        settings.radius_x = settings.radius_y = radius;
        settings.composite = CompositeMode::kGlowOnly;

        const int expansion = static_cast<int>(std::ceil(abglow::GlowReach(settings, layer_w, layer_h)));
        const int dest_w = layer_w + 2 * expansion;
        const int dest_h = layer_h + 2 * expansion;

        TestImage source(layer_w, layer_h, PixelDepth::kFloat32);
        TestImage dest(dest_w, dest_h, PixelDepth::kFloat32);
        for (int y = 0; y < layer_h; ++y) {
            for (int x = 0; x < layer_w; ++x) source.SetPixel(x, y, PixelF{1.0f, 1.0f, 1.0f, 1.0f});
        }

        GlowRender render;
        render.source = source.View();
        render.dest = dest.View();
        render.source_offset_x = -expansion;
        render.source_offset_y = -expansion;
        Check(abglow::RenderGlow(settings, render, allocator, runner) == GlowResult::kOk,
              "slide render succeeds");

        // Centroid relative to the layer's own centre.
        const float centre = GlowCentroid(dest, expansion + layer_h / 2) - (expansion + layer_w / 2.0f);
        if (previous > -1000.0f && previous != -1.0f) worst = std::max(worst, std::fabs(centre - previous));
        previous = centre;
    }
    Check(worst < 0.25f, std::string("glow stays put as the radius animates") + ModelName(model));
}

// The same glow, rendered at each of After Effects' resolutions, must come out
// the same size in composition space.
void TestSizeIsResolutionIndependent(GlowModel model) {
    MallocAllocator allocator;
    ThreadPoolRunner runner(4);

    const float radius_comp = 600.0f;
    const int layer_w_comp = 320;
    const int layer_h_comp = 120;
    float widest = 0.0f;
    float narrowest = 1e9f;

    for (int den : {1, 2, 3, 4}) {
        const float radius = radius_comp / static_cast<float>(den);
        const int layer_w = layer_w_comp / den;
        const int layer_h = layer_h_comp / den;

        GlowSettings settings = DefaultSettings();
        settings.model = model;
        settings.resolution = 1.0f / static_cast<float>(den);
        settings.threshold = 0.0f;
        settings.radius_x = settings.radius_y = radius;
        settings.composite = CompositeMode::kGlowOnly;

        const int expansion = static_cast<int>(std::ceil(abglow::GlowReach(settings, layer_w, layer_h)));
        const int dest_w = layer_w + 2 * expansion;
        const int dest_h = layer_h + 2 * expansion;

        TestImage source(layer_w, layer_h, PixelDepth::kFloat32);
        TestImage dest(dest_w, dest_h, PixelDepth::kFloat32);
        for (int y = 0; y < layer_h; ++y) {
            for (int x = 0; x < layer_w; ++x) source.SetPixel(x, y, PixelF{1.0f, 1.0f, 1.0f, 1.0f});
        }

        GlowRender render;
        render.source = source.View();
        render.dest = dest.View();
        render.source_offset_x = -expansion;
        render.source_offset_y = -expansion;
        Check(abglow::RenderGlow(settings, render, allocator, runner) == GlowResult::kOk,
              "resolution render succeeds");

        const float width = GlowWidth(dest, expansion + layer_h / 2, 0.5f) * static_cast<float>(den);
        Check(width > 0.0f, "glow fits inside the expanded bounds");
        widest = std::max(widest, width);
        narrowest = std::min(narrowest, width);
    }
    Check(widest <= narrowest * 1.02f,
          std::string("glow is the same size at every render resolution") + ModelName(model));
}

// After Effects renders a reduced-resolution preview from a downsampled layer,
// so anti-aliased edges carry the same light in fewer, partially covered
// pixels. Extraction has to stay linear in coverage or the same glow comes out
// dimmer at Half and Quarter - thresholding the premultiplied value made it 8%
// dimmer at Quarter on text.
void TestBrightnessIsResolutionIndependent(GlowModel model, AlphaMode alpha) {
    MallocAllocator allocator;
    ThreadPoolRunner runner(4);

    const int comp_w = 640;
    const int comp_h = 360;
    const float radius_comp = 120.0f;
    double brightest = 0.0;
    double dimmest = 1e30;

    for (int den : {1, 2, 4}) {
        const int width = comp_w / den;
        const int height = comp_h / den;
        TestImage source(width, height, PixelDepth::kFloat32, alpha);
        TestImage dest(width, height, PixelDepth::kFloat32);

        // A stroke whose edges land off the pixel grid at every resolution, so
        // the coverage really is spread over partial pixels.
        const float left = 260.0f;
        const float right = 274.5f;
        for (int y = height / 3; y < 2 * height / 3; ++y) {
            for (int x = 0; x < width; ++x) {
                const float x0 = static_cast<float>(x * den);
                const float x1 = x0 + static_cast<float>(den);
                const float covered = std::max(0.0f, std::min(x1, right) - std::max(x0, left));
                const float coverage = covered / static_cast<float>(den);
                if (coverage <= 0.0f) continue;
                // After Effects stores the colour of an edge pixel as it is and
                // its coverage in alpha; premultiplied, the colour is scaled.
                const float colour = alpha == AlphaMode::kStraight ? 1.0f : coverage;
                source.SetPixel(x, y, PixelF{coverage, 0.0f, colour, colour});
            }
        }

        GlowSettings settings = DefaultSettings();
        settings.model = model;
        settings.resolution = 1.0f / static_cast<float>(den);
        settings.threshold = 0.5f;
        settings.threshold_softness = 0.69f;
        settings.radius_x = settings.radius_y = radius_comp / static_cast<float>(den);
        settings.composite = CompositeMode::kGlowOnly;
        settings.working_space = WorkingSpace::kLinear;

        GlowRender render;
        render.source = source.View();
        render.dest = dest.View();
        Check(abglow::RenderGlow(settings, render, allocator, runner) == GlowResult::kOk,
              "brightness render succeeds");

        double energy = 0.0;
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) energy += dest.GetPixel(x, y).b;
        }
        energy *= static_cast<double>(den) * static_cast<double>(den);
        Check(energy > 0.0, "glow carries light at every resolution");
        brightest = std::max(brightest, energy);
        dimmest = std::min(dimmest, energy);
    }
    Check(brightest <= dimmest * 1.02,
          std::string("glow is the same brightness at every render resolution") + ModelName(model) +
              (alpha == AlphaMode::kStraight ? " from straight pixels" : " from premultiplied pixels"));
}

// Dither must not invent light. With expanded bounds most of the output buffer
// is untouched black, and a +-1 LSB triangular dither rounds an exact zero up a
// quarter of the time, which showed as neutral speckle scattered around the
// layer. It must also be keyed to the layer, not to the output buffer: the
// expanded rect moves as a layer's Position animates, so a buffer-keyed pattern
// crawls over the whole frame and shimmers.
void TestDitherIsQuietAndStill() {
    MallocAllocator allocator;
    ThreadPoolRunner runner(4);

    const int layer_w = 160;
    const int layer_h = 100;
    const float radius = 120.0f;
    const int expansion = static_cast<int>(
        std::ceil(abglow::MakeGlowPlan(abglow::RadiusToSigma(radius), Quality::kNormal, layer_w, layer_h).Reach()));

    TestImage source(layer_w, layer_h, PixelDepth::kBits8);
    for (int y = 15; y < layer_h - 15; ++y) {
        for (int x = 15; x < layer_w - 15; ++x) source.SetPixel(x, y, PixelF{1.0f, 1.0f, 0.29f, 0.29f});
    }

    GlowSettings settings = DefaultSettings();
    settings.threshold = 0.5f;
    settings.radius_x = settings.radius_y = radius;
    settings.intensity = 0.09f;
    settings.exposure = 4.78f;
    settings.dither = true;

    auto render_at = [&](int extra, TestImage& dest) {
        dest.Resize(layer_w + 2 * expansion + extra, layer_h + 2 * expansion + extra, PixelDepth::kBits8);
        GlowRender render;
        render.source = source.View();
        render.dest = dest.View();
        render.source_offset_x = -expansion - extra;
        render.source_offset_y = -expansion - extra;
        Check(abglow::RenderGlow(settings, render, allocator, runner) == GlowResult::kOk,
              "dither render succeeds");
    };

    TestImage a;
    TestImage b;
    render_at(0, a);
    render_at(1, b);

    // The far corner of the expanded rect carries no glow worth a code value.
    int lifted = 0;
    int corner = 0;
    for (int y = 2; y < 40; ++y) {
        for (int x = 2; x < 40; ++x) {
            const PixelF p = a.GetPixel(x, y);
            ++corner;
            if (std::lround(p.r * 255.0f) || std::lround(p.g * 255.0f) || std::lround(p.b * 255.0f)) ++lifted;
        }
    }
    Check(lifted * 20 < corner, "dither leaves untouched black alone");

    // Shifting the buffer by a pixel must shift the render, not reshuffle it.
    int differing = 0;
    int total = 0;
    long worst = 0;
    for (int y = 0; y < layer_h + 2 * expansion; ++y) {
        for (int x = 0; x < layer_w + 2 * expansion; ++x) {
            const PixelF p = a.GetPixel(x, y);
            const PixelF q = b.GetPixel(x + 1, y + 1);
            ++total;
            const long dr = std::labs(std::lround(p.r * 255.0f) - std::lround(q.r * 255.0f));
            const long dg = std::labs(std::lround(p.g * 255.0f) - std::lround(q.g * 255.0f));
            const long db = std::labs(std::lround(p.b * 255.0f) - std::lround(q.b * 255.0f));
            const long d = std::max(dr, std::max(dg, db));
            if (d != 0) ++differing;
            worst = std::max(worst, d);
        }
    }
    Check(worst <= 1, "moving the layer never shifts a pixel by more than one code value");
    Check(differing * 10 < total, "dither does not crawl when the layer moves");
}

// A host renders only the region it needs - the visible part of a zoomed
// viewer, a region of interest, a tile - so the same glow gets asked for
// through windows of every size and position. What comes back has to be the
// same pixels either way. Sizing the pyramid to the request instead of to the
// glow discarded source pixels outside the window and made the blur clamp
// against its edge, which changed the glow with the viewer.
void TestRegionOfInterestMatchesFullFrame(GlowModel model, float aspect = 1.0f) {
    MallocAllocator allocator;
    ThreadPoolRunner runner(4);

    const int layer_w = 400;
    const int layer_h = 300;
    GlowSettings settings = DefaultSettings();
    settings.model = model;
    settings.threshold = 0.5f;
    settings.composite = CompositeMode::kGlowOnly;
    settings.radius_x = settings.radius_y = 300.0f;
    settings.aspect_ratio = aspect;
    if (model == GlowModel::kInverseSquare) {
        // Far enough, on a small enough budget, that the pyramid splits into a
        // core tier over the layer and a halo tier over the reach.
        settings.radius_x = settings.radius_y = 400.0f;
        settings.quality = Quality::kDraft;
        Check(abglow::PlanForLayer(settings, layer_w, layer_h).split_level > 0,
              "the window test covers a two-tier pyramid");
    }
    const int expansion = static_cast<int>(std::ceil(abglow::GlowReach(settings, layer_w, layer_h)));

    TestImage source(layer_w, layer_h, PixelDepth::kFloat32);
    for (int y = 100; y < 200; ++y) {
        for (int x = 150; x < 250; ++x) source.SetPixel(x, y, PixelF{1.0f, 4.0f, 4.0f, 4.0f});
    }

    const int full_w = layer_w + 2 * expansion;
    const int full_h = layer_h + 2 * expansion;
    TestImage full(full_w, full_h, PixelDepth::kFloat32);
    GlowRender render;
    render.source = source.View();
    render.dest = full.View();
    render.source_offset_x = -expansion;
    render.source_offset_y = -expansion;
    Check(abglow::RenderGlow(settings, render, allocator, runner) == GlowResult::kOk, "full render succeeds");

    struct Window {
        int x, y, w, h;
    };
    const Window windows[] = {{expansion - 40, expansion - 40, layer_w + 80, layer_h + 80},
                              {0, 0, 200, 150},
                              {full_w - 260, full_h - 190, 260, 190},
                              {expansion, expansion, layer_w, layer_h}};

    for (const Window& win : windows) {
        TestImage part(win.w, win.h, PixelDepth::kFloat32);
        GlowRender sub;
        sub.source = source.View();
        sub.dest = part.View();
        sub.source_offset_x = -expansion + win.x;
        sub.source_offset_y = -expansion + win.y;
        Check(abglow::RenderGlow(settings, sub, allocator, runner) == GlowResult::kOk, "window render succeeds");

        float worst = 0.0f;
        for (int y = 0; y < win.h; ++y) {
            for (int x = 0; x < win.w; ++x) {
                const float a = full.GetPixel(win.x + x, win.y + y).g;
                const float b = part.GetPixel(x, y).g;
                const float scale = std::max(std::fabs(a), 1e-4f);
                worst = std::max(worst, std::fabs(b - a) / scale);
            }
        }
        Check(worst < 1e-3f,
              std::string("a requested window matches the same pixels of the full render") + ModelName(model));
    }
}

// Decimating the highlights with a box preserves their light but not their
// centre of mass, so a shape crossing the pyramid grid made the glow lead and
// lag by up to a twelfth of a cell - a sawtooth with the period of the pyramid
// step, which is the shimmer seen on a moving layer. A tent prefilter
// reproduces linear functions, so the centroid survives the decimation.
void TestGlowTracksSubPixelMotion(GlowModel model) {
    MallocAllocator allocator;
    ThreadPoolRunner runner(4);

    const int width = 2400;
    const int height = 2000;
    const float radius = 240.0f;
    float worst = 0.0f;

    for (int step = 0; step <= 16; ++step) {
        const double shift = step * 0.25;
        const double left = 1150.0 + shift;
        const double right = left + 40.0;

        TestImage source(width, height, PixelDepth::kFloat32);
        for (int y = height / 2 - 20; y < height / 2 + 20; ++y) {
            for (int x = 1140; x < 1200; ++x) {
                const double covered =
                    std::max(0.0, std::min(static_cast<double>(x) + 1.0, right) - std::max(static_cast<double>(x), left));
                if (covered <= 0.0) continue;
                const float a = static_cast<float>(covered);
                source.SetPixel(x, y, PixelF{a, 8.0f * a, 8.0f * a, 8.0f * a});
            }
        }

        GlowSettings settings = DefaultSettings();
        settings.model = model;
        settings.threshold = 0.5f;
        settings.radius_x = settings.radius_y = radius;
        settings.composite = CompositeMode::kGlowOnly;
        settings.working_space = WorkingSpace::kLinear;
        settings.dither = false;

        TestImage dest(width, height, PixelDepth::kFloat32);
        GlowRender render;
        render.source = source.View();
        render.dest = dest.View();
        Check(abglow::RenderGlow(settings, render, allocator, runner) == GlowResult::kOk,
              "sub-pixel render succeeds");

        double source_mass = 0.0;
        double source_moment = 0.0;
        double glow_mass = 0.0;
        double glow_moment = 0.0;
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const double sv = source.GetPixel(x, y).g;
                source_mass += sv;
                source_moment += sv * (x + 0.5);
                const double gv = dest.GetPixel(x, y).g;
                glow_mass += gv;
                glow_moment += gv * (x + 0.5);
            }
        }
        Check(source_mass > 0.0 && glow_mass > 0.0, "sub-pixel test carries light");
        const double drift = (glow_moment / glow_mass) - (source_moment / source_mass);
        worst = std::max(worst, static_cast<float>(std::fabs(drift)));
    }
    Check(worst < 0.05f, std::string("glow follows the source through sub-pixel motion") + ModelName(model));
}

// Clipping each channel on its own reaches the ceiling at a different
// brightness per channel, so an over-driven saturated colour drifts to white.
// Rolling the triple off together keeps the ratios between channels.
void TestHighlightRolloffKeepsHue() {
    MallocAllocator allocator;
    ThreadPoolRunner runner(4);

    const int width = 500;
    const int height = 400;
    TestImage source(width, height, PixelDepth::kFloat32);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const bool inside = std::hypot(x - 250.0, y - 200.0) < 50.0;
            // A light red, so the weak channels are non-zero and can clip too.
            source.SetPixel(x, y, inside ? PixelF{1.0f, 1.0f, 0.08f, 0.08f} : PixelF{0.0f, 0.0f, 0.0f, 0.0f});
        }
    }

    auto hue_drift = [&](abglow::HighlightRolloff mode) {
        TestImage dest(width, height, PixelDepth::kBits8);
        GlowSettings settings = DefaultSettings();
        settings.threshold = 0.3f;
        settings.radius_x = settings.radius_y = 110.0f;
        settings.intensity = 60.0f;
        settings.composite = CompositeMode::kGlowOnly;
        settings.rolloff = mode;
        GlowRender render;
        render.source = source.View();
        render.dest = dest.View();
        Check(abglow::RenderGlow(settings, render, allocator, runner) == GlowResult::kOk,
              "rolloff render succeeds");
        // In the blown-out core, how close to neutral has the colour gone?
        const PixelF p = dest.GetPixel(250, 200);
        const float mx = std::max(p.r, std::max(p.g, p.b));
        const float mn = std::min(p.r, std::min(p.g, p.b));
        return mx > 0.0f ? mn / mx : 0.0f;
    };

    const float clipped = hue_drift(abglow::HighlightRolloff::kClip);
    const float preserved = hue_drift(abglow::HighlightRolloff::kPreserveHue);
    Check(clipped > 0.8f, "clipping does drive an over-driven colour towards neutral");
    Check(preserved < 0.35f, "preserving hue keeps an over-driven colour saturated");
}

// A bloom is a sum of octaves spanning from a fine scale up to the radius, all
// carrying real weight. Concentrating the weight on one scale instead gives a
// band-limited blur: a field of bright specks dissolves into a flat haze rather
// than each speck glowing, and a small bright shape comes out much dimmer than
// a large one at the same settings.
void TestGlowIsBloomNotBlur() {
    MallocAllocator allocator;
    ThreadPoolRunner runner(4);

    const int width = 900;
    const int height = 700;
    TestImage source(width, height, PixelDepth::kFloat32);
    unsigned seed = 12345u;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            seed = seed * 1664525u + 1013904223u;
            const float value = static_cast<float>((seed >> 16) & 0xFF) / 255.0f;
            const float lit = value > 0.72f ? 3.0f : 0.05f;
            source.SetPixel(x, y, PixelF{1.0f, lit, lit, lit});
        }
    }

    GlowSettings settings = DefaultSettings();
    settings.threshold = 0.5f;
    settings.radius_x = settings.radius_y = 60.0f;
    settings.intensity = 1.0f;
    settings.composite = CompositeMode::kGlowOnly;
    settings.working_space = WorkingSpace::kLinear;
    settings.dither = false;

    TestImage dest(width, height, PixelDepth::kFloat32);
    GlowRender render;
    render.source = source.View();
    render.dest = dest.View();
    Check(abglow::RenderGlow(settings, render, allocator, runner) == GlowResult::kOk, "bloom render succeeds");

    double mean = 0.0;
    int count = 0;
    for (int y = 100; y < height - 100; ++y) {
        for (int x = 100; x < width - 100; ++x) {
            mean += dest.GetPixel(x, y).g;
            ++count;
        }
    }
    mean /= count;
    double variance = 0.0;
    for (int y = 100; y < height - 100; ++y) {
        for (int x = 100; x < width - 100; ++x) {
            const double d = dest.GetPixel(x, y).g - mean;
            variance += d * d;
        }
    }
    variance /= count;
    Check(mean > 0.0 && std::sqrt(variance) / mean > 0.04,
          "individual highlights still glow instead of merging into a haze");
}

// The same settings on a short word and a long one should read as the same
// brightness. Some difference is inherent to a convolution, but when almost all
// the weight sits on scales wider than the shape it becomes severe.
void TestSmallAndLargeShapesGlowAlike() {
    MallocAllocator allocator;
    ThreadPoolRunner runner(4);

    const int width = 1800;
    const int height = 700;
    auto peak_for = [&](int bar_width) {
        TestImage source(width, height, PixelDepth::kFloat32);
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const bool inside = std::abs(x - 900) <= bar_width / 2 && std::abs(y - 350) <= 60;
                source.SetPixel(x, y, inside ? PixelF{1.0f, 3.0f, 3.0f, 3.0f} : PixelF{0.0f, 0.0f, 0.0f, 0.0f});
            }
        }
        GlowSettings settings = DefaultSettings();
        settings.threshold = 0.5f;
        settings.radius_x = settings.radius_y = 200.0f;
        settings.composite = CompositeMode::kGlowOnly;
        settings.working_space = WorkingSpace::kLinear;
        settings.dither = false;
        TestImage dest(width, height, PixelDepth::kFloat32);
        GlowRender render;
        render.source = source.View();
        render.dest = dest.View();
        Check(abglow::RenderGlow(settings, render, allocator, runner) == GlowResult::kOk,
              "shape-size render succeeds");
        return dest.GetPixel(900, 350).g;
    };

    const float small = peak_for(80);
    const float large = peak_for(600);
    Check(small > 0.0f && large / small < 1.35f, "a small shape glows about as brightly as a large one");
}

// Screen is only defined in [0,1]; a + b - a*b turns back down above it and
// goes negative, which put black pixels in the brightest part of an HDR glow.
void TestScreenStaysPositive() {
    MallocAllocator allocator;
    ThreadPoolRunner runner(4);

    const int width = 700;
    const int height = 500;
    TestImage source(width, height, PixelDepth::kFloat32);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const bool inside = std::hypot(x - 350.0, y - 250.0) < 70.0;
            source.SetPixel(x, y, inside ? PixelF{1.0f, 6.0f, 2.0f, 0.5f} : PixelF{0.0f, 0.0f, 0.0f, 0.0f});
        }
    }

    GlowSettings settings = DefaultSettings();
    settings.threshold = 0.4f;
    settings.radius_x = settings.radius_y = 140.0f;
    settings.intensity = 9.0f;
    settings.composite = CompositeMode::kScreen;
    settings.working_space = WorkingSpace::kLinear;
    settings.dither = false;

    TestImage dest(width, height, PixelDepth::kFloat32);
    GlowRender render;
    render.source = source.View();
    render.dest = dest.View();
    Check(abglow::RenderGlow(settings, render, allocator, runner) == GlowResult::kOk, "screen render succeeds");

    float lowest = 0.0f;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const PixelF p = dest.GetPixel(x, y);
            lowest = std::min(lowest, std::min(p.r, std::min(p.g, p.b)));
        }
    }
    Check(lowest >= 0.0f, "screen never drives a channel negative");
}

// Screen's ceiling has to be reached smoothly. Clamping the two operands
// before multiplying them changes the slope the moment the sum crosses 1.0,
// and a slope that jumps along a smooth gradient is drawn as a contour line -
// the hard-edged ring Screen used to put through a bright glow. The check is
// on the second difference, which spikes at a kink; Add over the same picture
// is the smooth reference. Screen only differs from Add where the source is
// lit too, so the shape sits on an opaque field rather than on black.
void TestScreenReachesItsCeilingSmoothly() {
    MallocAllocator allocator;
    ThreadPoolRunner runner(4);

    const int width = 900;
    const int height = 200;
    TestImage source(width, height, PixelDepth::kFloat32);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            // Field below the threshold, so only the bar emits.
            source.SetPixel(x, y, x < 60 ? PixelF{4.0f, 4.0f, 4.0f, 1.0f} : PixelF{0.2f, 0.2f, 0.2f, 1.0f});
        }
    }

    GlowSettings settings = DefaultSettings();
    settings.threshold = 0.3f;
    settings.radius_x = settings.radius_y = 220.0f;
    settings.intensity = 6.0f;
    settings.working_space = WorkingSpace::kLinear;
    settings.dither = false;

    // Worst second difference along the falling glow, divided by the first
    // difference there so the two composites are on the same footing.
    auto roughness = [&](CompositeMode mode) {
        settings.composite = mode;
        TestImage dest(width, height, PixelDepth::kFloat32);
        GlowRender render;
        render.source = source.View();
        render.dest = dest.View();
        Check(abglow::RenderGlow(settings, render, allocator, runner) == GlowResult::kOk,
              "screen continuity render succeeds");

        std::vector<float> line(static_cast<std::size_t>(width));
        for (int x = 0; x < width; ++x) line[static_cast<std::size_t>(x)] = dest.GetPixel(x, height / 2).g;

        float worst = 0.0f;
        for (int x = 100; x < width - 20; ++x) {
            const float a = line[static_cast<std::size_t>(x - 1)];
            const float b = line[static_cast<std::size_t>(x)];
            const float c = line[static_cast<std::size_t>(x + 1)];
            const float slope = std::fabs(c - a) * 0.5f;
            if (slope < 1.0e-6f) continue;
            worst = std::max(worst, std::fabs(a - 2.0f * b + c) / slope);
        }
        return worst;
    };

    const float add = roughness(CompositeMode::kAdd);
    const float screen = roughness(CompositeMode::kScreen);
    Check(add > 0.0f, "the add reference has a gradient to compare against");
    // The hard clamp scored 4.1x add here; the smooth join scores 0.94x.
    Check(screen < add * 2.0f, "screen bends no harder than add where the glow crosses 1.0");
}

// Veiling glare follows a power law - the Stiles-Holladay form used in the CIE
// disability-glare equations is 1/theta^2. A sum of Gaussians whose sigmas
// double reproduces 1/r^n when the octave weights go as sigma^(2-n); this
// checks the rendered profile really lands on the exponent that was asked for.
void TestFalloffFollowsThePowerLaw() {
    MallocAllocator allocator;
    ThreadPoolRunner runner(4);

    const int size = 1600;
    for (float falloff : {2.0f, 2.5f, 3.0f}) {
        TestImage source(size, size, PixelDepth::kFloat32);
        source.SetPixel(size / 2, size / 2, PixelF{1.0f, 500.0f, 500.0f, 500.0f});

        GlowSettings settings = DefaultSettings();
        settings.threshold = 0.0f;
        settings.radius_x = settings.radius_y = 300.0f;
        settings.composite = CompositeMode::kGlowOnly;
        settings.working_space = WorkingSpace::kLinear;
        settings.dither = false;
        settings.falloff = falloff;

        TestImage dest(size, size, PixelDepth::kFloat32);
        GlowRender render;
        render.source = source.View();
        render.dest = dest.View();
        Check(abglow::RenderGlow(settings, render, allocator, runner) == GlowResult::kOk,
              "falloff render succeeds");

        const abglow::GlowPlan plan =
            abglow::MakeGlowPlan(abglow::RadiusToSigma(settings.radius_x), settings.quality, size, size, 1, falloff);
        // Below the cutoff at the radius, which is where the law applies.
        const double low = 3.0 * plan.effective_sigma[0] * plan.base_scale;
        const double high = 0.35 * plan.effective_sigma[plan.level_count - 1] * plan.base_scale;

        // Least squares on log intensity against log radius.
        double sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0;
        int n = 0;
        for (double r = low; r <= high; r *= 1.06) {
            const double v = dest.GetPixel(size / 2 + static_cast<int>(std::lround(r)), size / 2).g;
            if (v <= 0.0) continue;
            const double lr = std::log(r);
            const double lv = std::log(v);
            sx += lr;
            sy += lv;
            sxx += lr * lr;
            sxy += lr * lv;
            ++n;
        }
        Check(n > 8, "falloff fit has samples");
        const double slope = -(n * sxy - sx * sy) / (n * sxx - sx * sx);
        Check(std::fabs(slope - falloff) < 0.15,
              "glow follows 1/r^" + std::to_string(static_cast<int>(falloff * 10)) + " as asked");
    }
}

// The ladder carries an octave past the radius, so the profile stays a power
// law there instead of turning into the widest octave's Gaussian shoulder and
// vanishing. The other half of that has to hold too: a large bright region must
// not lift the whole frame into haze.
void TestGlowHasALongTailWithoutHaze() {
    MallocAllocator allocator;
    ThreadPoolRunner runner(4);

    const float radius = 200.0f;
    {
        const int size = 2400;
        TestImage source(size, size, PixelDepth::kFloat32);
        for (int y = size / 2 - 25; y < size / 2 + 25; ++y) {
            for (int x = size / 2 - 25; x < size / 2 + 25; ++x) {
                source.SetPixel(x, y, PixelF{1.0f, 20.0f, 20.0f, 20.0f});
            }
        }
        GlowSettings settings = DefaultSettings();
        settings.threshold = 0.5f;
        settings.radius_x = settings.radius_y = radius;
        settings.composite = CompositeMode::kGlowOnly;
        settings.working_space = WorkingSpace::kLinear;
        settings.dither = false;

        TestImage dest(size, size, PixelDepth::kFloat32);
        GlowRender render;
        render.source = source.View();
        render.dest = dest.View();
        Check(abglow::RenderGlow(settings, render, allocator, runner) == GlowResult::kOk, "tail render succeeds");

        const float near = dest.GetPixel(size / 2 + static_cast<int>(radius * 0.25f), size / 2).g;
        const float far = dest.GetPixel(size / 2 + static_cast<int>(radius * 2.0f), size / 2).g;
        Check(near > 0.0f, "tail test carries light");
        Check(far > near * 1.0e-4f, "glow still carries light at twice the radius");
    }

    {
        // A large bright region, and an empty corner far from it.
        const int width = 2000;
        const int height = 1400;
        TestImage source(width, height, PixelDepth::kFloat32);
        for (int y = 150; y < 750; ++y) {
            for (int x = 150; x < 750; ++x) source.SetPixel(x, y, PixelF{1.0f, 6.0f, 6.0f, 6.0f});
        }
        GlowSettings settings = DefaultSettings();
        settings.threshold = 0.5f;
        settings.radius_x = settings.radius_y = radius;
        settings.composite = CompositeMode::kGlowOnly;
        settings.working_space = WorkingSpace::kLinear;
        settings.dither = false;

        TestImage dest(width, height, PixelDepth::kFloat32);
        GlowRender render;
        render.source = source.View();
        render.dest = dest.View();
        Check(abglow::RenderGlow(settings, render, allocator, runner) == GlowResult::kOk, "haze render succeeds");

        const double core = dest.GetPixel(450, 450).g;
        double corner = 0.0;
        int count = 0;
        for (int y = height - 300; y < height - 50; ++y) {
            for (int x = width - 300; x < width - 50; ++x) {
                corner += dest.GetPixel(x, y).g;
                ++count;
            }
        }
        corner /= count;
        Check(core > 0.0 && corner < core * 0.002, "a large bright region leaves no flat haze");
    }
}

// A lens does not focus every wavelength at the same distance, so its veiling
// glare is a slightly different size per channel. Multiply R/G/B scales each
// channel's radius; the widths of the rendered channels have to follow.
void TestGlowAberrationSpreadsTheChannels() {
    MallocAllocator allocator;
    ThreadPoolRunner runner(4);

    const int width = 512;
    const int height = 256;
    TestImage source(width, height, PixelDepth::kFloat32);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const bool inside = std::hypot(x - 256.0, y - 128.0) < 6.0;
            source.SetPixel(x, y, inside ? PixelF{1.0f, 3.0f, 3.0f, 3.0f} : PixelF{0.0f, 0.0f, 0.0f, 0.0f});
        }
    }

    GlowSettings settings = DefaultSettings();
    settings.threshold = 0.2f;
    settings.radius_x = settings.radius_y = 90.0f;
    settings.intensity = 1.0f;
    settings.composite = CompositeMode::kGlowOnly;
    settings.aberration_r = 1.4f;
    settings.aberration_b = 0.7f;

    TestImage dest(width, height, PixelDepth::kFloat32);
    GlowRender render;
    render.source = source.View();
    render.dest = dest.View();
    Check(abglow::RenderGlow(settings, render, allocator, runner) == GlowResult::kOk, "aberration render succeeds");

    // Out on the tail, where the octave weighting separates the channels;
    // near the core they are within a pixel of each other.
    auto extent = [&](int channel) {
        const PixelF peak = dest.GetPixel(256, 128);
        const float top = channel == 0 ? peak.r : channel == 1 ? peak.g : peak.b;
        for (int x = 256; x < width; ++x) {
            const PixelF p = dest.GetPixel(x, 128);
            const float v = channel == 0 ? p.r : channel == 1 ? p.g : p.b;
            if (v < top * 0.02f) return x - 256;
        }
        return width;
    };

    const int red = extent(0);
    const int green = extent(1);
    const int blue = extent(2);
    Check(red > green && green > blue, "a larger multiplier gives that channel a wider glow");
    Check(red - blue >= 5, "the channels are separated by more than a rounding step");

    // Same light spread over more area, so the wider channel peaks lower.
    const PixelF peak = dest.GetPixel(256, 128);
    Check(peak.r < peak.g && peak.g < peak.b, "a wider channel peaks lower for the same light");
}

// Source Opacity fades the layer the glow is composited over without touching
// the glow itself, which is what makes it usable for glow-on-its-own looks
// that still need the source's alpha.
void TestSourceOpacityLeavesTheGlowAlone() {
    MallocAllocator allocator;
    ThreadPoolRunner runner(4);

    const int width = 256;
    const int height = 128;
    TestImage source(width, height, PixelDepth::kFloat32);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const bool inside = std::abs(x - 128) < 8 && std::abs(y - 64) < 8;
            source.SetPixel(x, y, inside ? PixelF{1.0f, 2.0f, 2.0f, 2.0f} : PixelF{0.0f, 0.0f, 0.0f, 0.0f});
        }
    }

    GlowSettings settings = DefaultSettings();
    settings.threshold = 0.2f;
    settings.radius_x = settings.radius_y = 40.0f;

    auto render_with = [&](float opacity) {
        settings.source_opacity = opacity;
        TestImage dest(width, height, PixelDepth::kFloat32);
        GlowRender render;
        render.source = source.View();
        render.dest = dest.View();
        Check(abglow::RenderGlow(settings, render, allocator, runner) == GlowResult::kOk,
              "source opacity render succeeds");
        return dest;
    };

    const TestImage full = render_with(1.0f);
    const TestImage none = render_with(0.0f);

    // Far from the shape the output is glow only, so the two must agree.
    const PixelF halo_full = full.GetPixel(128, 100);
    const PixelF halo_none = none.GetPixel(128, 100);
    CheckNear(halo_none.g, halo_full.g, halo_full.g * 0.001f + 1.0e-6f, "opacity leaves the halo untouched");
    Check(halo_full.g > 0.0f, "there is a halo to compare");

    // On the shape it has removed the source and left the glow.
    const PixelF core_full = full.GetPixel(128, 64);
    const PixelF core_none = none.GetPixel(128, 64);
    Check(core_none.g < core_full.g - 1.5f, "opacity 0 takes the source out of the result");
    Check(core_none.g > 0.0f, "the glow is still there without the source");
}

// Unmult is for footage delivered on black with no usable alpha: coverage comes
// from how bright the pixel is rather than from an alpha that is 1 everywhere,
// so a dim opaque field emits in proportion to its brightness.
void TestUnmultReadsCoverageFromBrightness() {
    MallocAllocator allocator;
    ThreadPoolRunner runner(4);

    const int width = 128;
    const int height = 128;
    TestImage source(width, height, PixelDepth::kFloat32);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) source.SetPixel(x, y, PixelF{1.0f, 0.5f, 0.5f, 0.5f});
    }

    GlowSettings settings = DefaultSettings();
    settings.threshold = 0.2f;
    settings.threshold_softness = 0.0f;
    settings.radius_x = settings.radius_y = 20.0f;
    settings.composite = CompositeMode::kGlowOnly;

    auto alpha_with = [&](bool unmult) {
        settings.unmult = unmult;
        TestImage dest(width, height, PixelDepth::kFloat32);
        GlowRender render;
        render.source = source.View();
        render.dest = dest.View();
        Check(abglow::RenderGlow(settings, render, allocator, runner) == GlowResult::kOk, "unmult render succeeds");
        return dest.GetPixel(64, 64).a;
    };

    // contribution = (0.5 - 0.2) / 0.5 = 0.6; coverage is alpha (1.0) without
    // unmult and the level (0.5) with it.
    CheckNear(alpha_with(false), 0.6f, 0.03f, "without unmult an opaque field is fully covered");
    CheckNear(alpha_with(true), 0.3f, 0.03f, "unmult reads coverage from the brightest channel");
}

// Saturation Bias weights the extraction by how colourful a pixel is, so a
// saturated shape can be made to glow harder than a white one of the same
// brightness - or the other way round.
void TestSaturationBiasFavoursColour() {
    MallocAllocator allocator;
    ThreadPoolRunner runner(4);

    const int width = 384;
    const int height = 160;
    TestImage source(width, height, PixelDepth::kFloat32);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            PixelF p{0.0f, 0.0f, 0.0f, 0.0f};
            if (std::abs(x - 96) < 10 && std::abs(y - 80) < 10) p = PixelF{1.0f, 2.0f, 0.0f, 0.0f};   // red
            if (std::abs(x - 288) < 10 && std::abs(y - 80) < 10) p = PixelF{1.0f, 2.0f, 2.0f, 2.0f};  // white
            source.SetPixel(x, y, p);
        }
    }

    GlowSettings settings = DefaultSettings();
    settings.threshold = 0.2f;
    settings.radius_x = settings.radius_y = 40.0f;
    settings.composite = CompositeMode::kGlowOnly;

    auto ratio = [&](float bias) {
        settings.saturation_bias = bias;
        TestImage dest(width, height, PixelDepth::kFloat32);
        GlowRender render;
        render.source = source.View();
        render.dest = dest.View();
        Check(abglow::RenderGlow(settings, render, allocator, runner) == GlowResult::kOk,
              "saturation bias render succeeds");
        const float red = dest.GetPixel(96, 40).r;
        const float white = dest.GetPixel(288, 40).r;
        return white > 0.0f ? red / white : 0.0f;
    };

    const float neutral = ratio(0.0f);
    const float favour_colour = ratio(1.0f);
    const float favour_white = ratio(-0.6f);
    Check(neutral > 0.0f, "both shapes glow at bias 0");
    Check(favour_colour > neutral * 1.5f, "a positive bias lifts the saturated shape");
    Check(favour_white < neutral * 0.7f, "a negative bias holds it back");
}

// An anisotropic radius stretches the glow, but not by as much as it is asked
// to: the pyramid halves both axes together, so the resampling filter puts a
// floor under the narrow axis that the per-axis blur cannot get below. A 4:1
// radius renders about 2:1. This is the same limitation that keeps the glow
// from being exactly round on non-square pixels, and it is why there is no
// Aspect Ratio control yet - it would render about the square root of what its
// number said. Decimating each axis on its own schedule is the fix.
void TestAnisotropicRadius() {
    MallocAllocator allocator;
    ThreadPoolRunner runner(4);

    const int width = 512;
    const int height = 512;
    TestImage source(width, height, PixelDepth::kFloat32);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const bool inside = std::hypot(x - 256.0, y - 256.0) < 6.0;
            source.SetPixel(x, y, inside ? PixelF{1.0f, 3.0f, 3.0f, 3.0f} : PixelF{0.0f, 0.0f, 0.0f, 0.0f});
        }
    }

    GlowSettings settings = DefaultSettings();
    settings.threshold = 0.2f;
    settings.radius_x = 120.0f;
    settings.radius_y = 30.0f;
    settings.composite = CompositeMode::kGlowOnly;

    TestImage dest(width, height, PixelDepth::kFloat32);
    GlowRender render;
    render.source = source.View();
    render.dest = dest.View();
    Check(abglow::RenderGlow(settings, render, allocator, runner) == GlowResult::kOk, "anisotropic render succeeds");

    const float peak = dest.GetPixel(256, 256).g;
    auto extent = [&](int dx, int dy) {
        for (int i = 1; i < 256; ++i) {
            if (dest.GetPixel(256 + dx * i, 256 + dy * i).g < peak * 0.02f) return i;
        }
        return 256;
    };
    const float horizontal = static_cast<float>(extent(1, 0));
    const float vertical = static_cast<float>(extent(0, 1));
    Check(vertical > 0.0f, "the glow has a measurable height");
    Check(horizontal / vertical > 1.8f, "an anisotropic radius stretches the glow");
    // Tighten this once each axis is decimated on its own schedule.
    // Each axis is decimated on its own schedule, so the narrow one is not
    // smeared by resampling sized for the wide one. The 6 px disc itself
    // pads both extents, so the measured ratio sits a little under 4.
    Check(horizontal / vertical > 3.2f && horizontal / vertical < 4.8f, "the glow keeps the requested 4:1");
}

void TestThresholdAndPassThrough() {
    MallocAllocator allocator;
    ThreadPoolRunner runner(2);

    TestImage source(64, 64, PixelDepth::kBits8);
    TestImage dest(64, 64, PixelDepth::kBits8);
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) source.SetPixel(x, y, PixelF{1.0f, 0.2f, 0.2f, 0.2f});
    }

    GlowSettings settings = DefaultSettings();
    settings.threshold = 0.9f;
    settings.working_space = WorkingSpace::kSrgb;

    GlowRender render;
    render.source = source.View();
    render.dest = dest.View();
    Check(abglow::RenderGlow(settings, render, allocator, runner) == GlowResult::kOk, "threshold render succeeds");

    bool identical = true;
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            const PixelF a = source.GetPixel(x, y);
            const PixelF b = dest.GetPixel(x, y);
            if (a.r != b.r || a.g != b.g || a.b != b.b || a.a != b.a) identical = false;
        }
    }
    Check(identical, "below-threshold input passes through bit-exact");
}

void TestTransparentInput() {
    MallocAllocator allocator;
    ThreadPoolRunner runner(2);

    TestImage source(48, 48, PixelDepth::kFloat32, AlphaMode::kStraight);
    TestImage dest(48, 48, PixelDepth::kFloat32, AlphaMode::kStraight);
    // Transparent but with leftover RGB, as AE can deliver with
    // preserve_rgb_of_zero_alpha.
    for (int y = 0; y < 48; ++y) {
        for (int x = 0; x < 48; ++x) source.SetPixel(x, y, PixelF{0.0f, 1.0f, 1.0f, 1.0f});
    }

    GlowSettings settings = DefaultSettings();
    settings.threshold = 0.0f;

    GlowRender render;
    render.source = source.View();
    render.dest = dest.View();
    Check(abglow::RenderGlow(settings, render, allocator, runner) == GlowResult::kOk, "transparent render succeeds");

    bool unchanged = true;
    for (int y = 0; y < 48; ++y) {
        for (int x = 0; x < 48; ++x) {
            const PixelF p = dest.GetPixel(x, y);
            if (p.a != 0.0f || p.r != 1.0f) unchanged = false;
        }
    }
    Check(unchanged, "zero alpha generates no light and keeps its RGB");
}

void TestHdrNotClamped() {
    MallocAllocator allocator;
    ThreadPoolRunner runner(2);

    TestImage source(64, 64, PixelDepth::kFloat32);
    TestImage dest(64, 64, PixelDepth::kFloat32);
    for (int y = 24; y < 40; ++y) {
        for (int x = 24; x < 40; ++x) source.SetPixel(x, y, PixelF{1.0f, 50.0f, 50.0f, 50.0f});
    }

    GlowSettings settings = DefaultSettings();
    settings.threshold = 1.0f;
    settings.radius_x = settings.radius_y = 8.0f;

    GlowRender render;
    render.source = source.View();
    render.dest = dest.View();
    Check(abglow::RenderGlow(settings, render, allocator, runner) == GlowResult::kOk, "hdr render succeeds");
    Check(dest.GetPixel(32, 32).r > 50.0f, "HDR highlight survives and gains glow");
    Check(dest.GetPixel(32, 32).a <= 1.0f, "alpha stays in range");
}

void TestDepthConsistency() {
    MallocAllocator allocator;
    ThreadPoolRunner runner(2);

    const int size = 96;
    std::vector<float> centre_values;
    for (PixelDepth depth : {PixelDepth::kBits8, PixelDepth::kBits16, PixelDepth::kFloat32}) {
        TestImage source(size, size, depth);
        TestImage dest(size, size, depth);
        for (int y = 40; y < 56; ++y) {
            for (int x = 40; x < 56; ++x) source.SetPixel(x, y, PixelF{1.0f, 1.0f, 1.0f, 1.0f});
        }

        GlowSettings settings = DefaultSettings();
        settings.threshold = 0.2f;
        settings.radius_x = settings.radius_y = 20.0f;
        settings.composite = CompositeMode::kGlowOnly;
        settings.working_space = WorkingSpace::kSrgb;

        GlowRender render;
        render.source = source.View();
        render.dest = dest.View();
        Check(abglow::RenderGlow(settings, render, allocator, runner) == GlowResult::kOk, "depth render succeeds");
        centre_values.push_back(dest.GetPixel(70, 48).g);
    }
    CheckNear(centre_values[1], centre_values[0], 0.02f, "16 bpc matches 8 bpc");
    CheckNear(centre_values[2], centre_values[0], 0.02f, "32 bpc matches 8 bpc");
}

// With expanded bounds the destination is larger than the source and the source
// sits at a negative offset.
void TestExpandedBounds() {
    MallocAllocator allocator;
    ThreadPoolRunner runner(2);

    TestImage source(64, 64, PixelDepth::kFloat32);
    TestImage dest(128, 128, PixelDepth::kFloat32);
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) source.SetPixel(x, y, PixelF{1.0f, 2.0f, 2.0f, 2.0f});
    }

    GlowSettings settings = DefaultSettings();
    settings.threshold = 0.0f;
    settings.radius_x = settings.radius_y = 20.0f;

    GlowRender render;
    render.source = source.View();
    render.dest = dest.View();
    render.source_offset_x = -32;
    render.source_offset_y = -32;
    Check(abglow::RenderGlow(settings, render, allocator, runner) == GlowResult::kOk, "expanded render succeeds");

    Check(dest.GetPixel(64, 64).r > 2.0f, "source lands at the expected offset");
    Check(dest.GetPixel(20, 64).r > 0.0f, "glow spreads into the expanded margin");
    Check(dest.GetPixel(1, 1).r < dest.GetPixel(20, 64).r, "margin falls off towards the edge");
    Check(dest.GetPixel(31, 64).a == 0.0f || dest.GetPixel(31, 64).a > 0.0f, "margin alpha is defined");
}

// With an offset source, untouched pixels must be copied from the matching
// source pixel, not from the destination's own column.
void TestOffsetPassThrough() {
    MallocAllocator allocator;
    ThreadPoolRunner runner(4);

    const int src_w = 240;
    const int src_h = 160;
    const int margin = 40;
    TestImage source(src_w, src_h, PixelDepth::kBits8);
    TestImage dest(src_w + 2 * margin, src_h + 2 * margin, PixelDepth::kBits8);

    // A dim gradient that stays below the threshold, plus one bright block.
    for (int y = 0; y < src_h; ++y) {
        for (int x = 0; x < src_w; ++x) {
            const float v = static_cast<float>(x) / static_cast<float>(src_w) * 0.25f;
            source.SetPixel(x, y, PixelF{1.0f, v, v * 0.5f, v * 0.25f});
        }
    }
    for (int y = 10; y < 30; ++y) {
        for (int x = 10; x < 30; ++x) source.SetPixel(x, y, PixelF{1.0f, 1.0f, 1.0f, 1.0f});
    }

    GlowSettings settings = DefaultSettings();
    settings.threshold = 0.8f;
    settings.radius_x = settings.radius_y = 12.0f;
    settings.working_space = WorkingSpace::kSrgb;

    GlowRender render;
    render.source = source.View();
    render.dest = dest.View();
    render.source_offset_x = -margin;
    render.source_offset_y = -margin;
    Check(abglow::RenderGlow(settings, render, allocator, runner) == GlowResult::kOk, "offset render succeeds");

    // Far from the bright block the gradient must survive untouched.
    bool matches = true;
    for (int y = src_h / 2; y < src_h; ++y) {
        for (int x = 100; x < src_w; ++x) {
            const PixelF expected = source.GetPixel(x, y);
            const PixelF actual = dest.GetPixel(x + margin, y + margin);
            if (expected.r != actual.r || expected.g != actual.g || expected.b != actual.b) matches = false;
        }
    }
    Check(matches, "offset source pixels pass through unchanged");
    Check(dest.GetPixel(5, 5).a == 0.0f, "the far corner of the margin stays empty");
}

void TestEmptySourceAndBadArguments() {
    MallocAllocator allocator;
    ThreadPoolRunner runner(1);

    TestImage dest(32, 32, PixelDepth::kBits8);
    GlowSettings settings = DefaultSettings();

    GlowRender render;
    render.dest = dest.View();
    Check(abglow::RenderGlow(settings, render, allocator, runner) == GlowResult::kOk, "empty source is handled");

    GlowRender broken;
    Check(abglow::RenderGlow(settings, broken, allocator, runner) == GlowResult::kInvalidArguments,
          "missing destination is rejected");
    Check(allocator.allocations() == allocator.frees(), "no leaks after error paths");
}

void TestZeroRadiusAndZeroIntensity() {
    MallocAllocator allocator;
    ThreadPoolRunner runner(2);

    TestImage source(48, 48, PixelDepth::kBits16);
    TestImage dest(48, 48, PixelDepth::kBits16);
    for (int y = 0; y < 48; ++y) {
        for (int x = 0; x < 48; ++x) source.SetPixel(x, y, PixelF{1.0f, 0.9f, 0.4f, 0.1f});
    }

    GlowSettings settings = DefaultSettings();
    settings.radius_x = settings.radius_y = 0.0f;
    settings.intensity = 0.0f;
    settings.threshold = 0.0f;

    GlowRender render;
    render.source = source.View();
    render.dest = dest.View();
    Check(abglow::RenderGlow(settings, render, allocator, runner) == GlowResult::kOk, "zero radius render succeeds");

    bool identical = true;
    for (int y = 0; y < 48; ++y) {
        for (int x = 0; x < 48; ++x) {
            const PixelF a = source.GetPixel(x, y);
            const PixelF b = dest.GetPixel(x, y);
            if (a.r != b.r || a.g != b.g || a.b != b.b) identical = false;
        }
    }
    Check(identical, "zero intensity leaves the image untouched");

    // The same fast path with an offset destination.
    TestImage offset_dest(48 + 20, 48 + 20, PixelDepth::kBits16);
    render.dest = offset_dest.View();
    render.source_offset_x = -10;
    render.source_offset_y = -10;
    Check(abglow::RenderGlow(settings, render, allocator, runner) == GlowResult::kOk, "offset copy succeeds");
    Check(offset_dest.GetPixel(30, 30).r == source.GetPixel(20, 20).r, "offset copy lands in the right place");
    Check(offset_dest.GetPixel(2, 2).a == 0.0f, "offset copy leaves the margin empty");
}

// Inverse Square puts the same light in every octave from the core to the
// radius, cut off above it. The weights therefore do not sum to one: the total
// grows with the log of the radius. The pyramid step must come from the layer
// and the quality alone - a step that moved with the radius would resample the
// fixed-size core and make it pop as the radius animates.
void TestInverseSquarePlan() {
    const int layer_w = 1920;
    const int layer_h = 1080;
    GlowSettings settings = DefaultSettings();
    settings.model = GlowModel::kInverseSquare;
    settings.falloff = 2.0f;

    int first_step = -1;
    float previous_total = 0.0f;
    for (float radius = 10.0f; radius <= 4000.0f; radius *= 1.25f) {
        settings.radius_x = settings.radius_y = radius;
        const abglow::GlowPlan plan = abglow::PlanForLayer(settings, layer_w, layer_h);
        Check(plan.level_count >= 1 && plan.level_count <= abglow::kMaxPyramidLevels, "plan level count in range");
        Check(plan.split_level >= 0 && plan.split_level < plan.level_count, "the halo tier starts on a level");
        if (first_step < 0) first_step = plan.base_scale;
        Check(plan.base_scale == first_step, "the pyramid step does not move with the radius");
        // The widest octave reaches past the radius, where the law is cut off.
        Check(plan.effective_sigma[plan.level_count - 1] * plan.base_scale >=
                  2.0f * abglow::RadiusToSigma(radius) * 0.999f,
              "the ladder reaches past the radius");

        float total = 0.0f;
        for (int i = 0; i < plan.level_count; ++i) total += plan.weights[i];
        Check(total > previous_total, "a larger radius adds light rather than spreading the same light thinner");
        previous_total = total;

        // Well inside the radius every octave carries the same light.
        if (radius >= 400.0f) {
            for (int i = 2; i < 5; ++i) {
                CheckNear(plan.weights[i], abglow::kInverseSquareOctaveGain,
                          abglow::kInverseSquareOctaveGain * 0.1f, "every octave below the radius carries the same light");
            }
        }
    }

    // Falloff moves light between the core and the halo; it does not change
    // how much there is.
    settings.radius_x = settings.radius_y = 300.0f;
    float totals[3] = {};
    float cores[3] = {};
    const float falloffs[3] = {1.5f, 2.0f, 3.0f};
    for (int k = 0; k < 3; ++k) {
        settings.falloff = falloffs[k];
        const abglow::GlowPlan plan = abglow::PlanForLayer(settings, layer_w, layer_h);
        for (int i = 0; i < plan.level_count; ++i) totals[k] += plan.weights[i];
        cores[k] = plan.weights[0];
    }
    CheckNear(totals[0], totals[1], totals[1] * 1e-3f, "falloff leaves the total light alone");
    CheckNear(totals[2], totals[1], totals[1] * 1e-3f, "falloff leaves the total light alone");
    Check(cores[0] < cores[1] && cores[1] < cores[2], "a steeper falloff puts more of the light in the core");
}

// Below its cutoff the inverse-square glow is a power law, and the exponent is
// the Falloff that was asked for.
void TestInverseSquareFollowsThePowerLaw() {
    MallocAllocator allocator;
    ThreadPoolRunner runner(4);

    const int size = 1600;
    for (float falloff : {1.5f, 2.0f, 3.0f}) {
        TestImage source(size, size, PixelDepth::kFloat32);
        source.SetPixel(size / 2, size / 2, PixelF{1.0f, 500.0f, 500.0f, 500.0f});

        GlowSettings settings = DefaultSettings();
        settings.model = GlowModel::kInverseSquare;
        settings.threshold = 0.0f;
        settings.radius_x = settings.radius_y = 600.0f;
        settings.composite = CompositeMode::kGlowOnly;
        settings.falloff = falloff;

        TestImage dest(size, size, PixelDepth::kFloat32);
        GlowRender render;
        render.source = source.View();
        render.dest = dest.View();
        Check(abglow::RenderGlow(settings, render, allocator, runner) == GlowResult::kOk,
              "inverse-square falloff render succeeds");

        // From clear of the core to half the radius sigma, where the cutoff has
        // not started to bite.
        const abglow::GlowPlan plan = abglow::PlanForRender(settings, render);
        const double low = 4.0 * plan.effective_sigma[0] * plan.base_scale;
        const double high = 0.5 * abglow::RadiusToSigma(settings.radius_x);
        double sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0;
        int n = 0;
        for (double r = low; r <= high; r *= 1.06) {
            const double v = dest.GetPixel(size / 2 + static_cast<int>(std::lround(r)), size / 2).g;
            if (v <= 0.0) continue;
            const double lr = std::log(r);
            const double lv = std::log(v);
            sx += lr;
            sy += lv;
            sxx += lr * lr;
            sxy += lr * lv;
            ++n;
        }
        Check(n > 8, "inverse-square fit has samples");
        const double slope = -(n * sxy - sx * sy) / (n * sxx - sx * sx);
        // Measured 1.571 / 2.037 / 3.008.
        Check(std::fabs(slope - falloff) < 0.12, "inverse-square glow follows 1/r^" +
                                                     std::to_string(static_cast<int>(falloff * 10)) + " as asked");
    }
}

// Under Inverse Square the radius is how far the light reaches. Quadrupling it
// has to carry the glow much further out while the hot core hugging the shape
// stays close to what it was - the opposite of a blur, whose core dims as it
// spreads.
void TestRadiusExtendsTheReachNotTheCore() {
    MallocAllocator allocator;
    ThreadPoolRunner runner(4);

    const int width = 1400;
    const int height = 900;
    TestImage source(width, height, PixelDepth::kFloat32);
    for (int y = 440; y < 460; ++y) {
        for (int x = 300; x < 1100; ++x) source.SetPixel(x, y, PixelF{1.0f, 1.0f, 1.0f, 1.0f});
    }

    auto profile = [&](GlowModel model, float radius, float* near, float* far) {
        GlowSettings settings = DefaultSettings();
        settings.model = model;
        settings.falloff = 2.0f;
        settings.threshold = 0.0f;
        settings.radius_x = settings.radius_y = radius;
        settings.composite = CompositeMode::kGlowOnly;
        TestImage dest(width, height, PixelDepth::kFloat32);
        GlowRender render;
        render.source = source.View();
        render.dest = dest.View();
        Check(abglow::RenderGlow(settings, render, allocator, runner) == GlowResult::kOk,
              "reach render succeeds");
        *near = dest.GetPixel(700, 461).g;
        *far = dest.GetPixel(700, 609).g;
    };

    float near_small = 0.0f, far_small = 0.0f, near_large = 0.0f, far_large = 0.0f;
    profile(GlowModel::kInverseSquare, 100.0f, &near_small, &far_small);
    profile(GlowModel::kInverseSquare, 400.0f, &near_large, &far_large);
    // Measured: 2 px out 0.688 -> 0.809, 150 px out 0.0004 -> 0.012.
    Check(near_small > 0.5f, "the core next to the shape is bright");
    Check(near_large > near_small && near_large < near_small * 1.3f, "a larger radius keeps the core");
    Check(far_large > far_small * 10.0f, "a larger radius carries the light further");

    profile(GlowModel::kClassic, 100.0f, &near_small, &far_small);
    profile(GlowModel::kClassic, 400.0f, &near_large, &far_large);
    Check(near_large < near_small, "classic spreads the same light thinner as the radius grows");
}

// The core is a fixed size, so nothing about the pyramid it is sampled on may
// change as the radius animates. The step is fixed by the layer; the halo tier
// starts wherever the budget needs, which changes only where the extents run,
// not what is in them. A radius sweep across those changes must stay smooth
// right next to the shape - folding the core into a coarser step used to make
// it jump by a fifth there.
void TestInverseSquareDoesNotPop() {
    MallocAllocator allocator;
    ThreadPoolRunner runner(4);

    const int width = 1400;
    const int height = 800;
    TestImage source(width, height, PixelDepth::kFloat32);
    for (int y = 390; y < 410; ++y) {
        for (int x = 300; x < 1100; ++x) source.SetPixel(x, y, PixelF{1.0f, 1.0f, 1.0f, 1.0f});
    }

    GlowSettings settings = DefaultSettings();
    settings.model = GlowModel::kInverseSquare;
    settings.falloff = 2.0f;
    settings.threshold = 0.0f;
    settings.composite = CompositeMode::kGlowOnly;

    const int distances[] = {1, 2, 4, 8, 16};
    float previous[5] = {};
    float worst = 0.0f;
    int splits_seen = 0;
    int last_split = -1;
    for (float radius = 180.0f; radius <= 260.0f; radius += 4.0f) {
        settings.radius_x = settings.radius_y = radius;
        TestImage dest(width, height, PixelDepth::kFloat32);
        GlowRender render;
        render.source = source.View();
        render.dest = dest.View();
        Check(abglow::RenderGlow(settings, render, allocator, runner) == GlowResult::kOk, "sweep render succeeds");
        const int split = abglow::PlanForRender(settings, render).split_level;
        if (split != last_split) ++splits_seen;
        last_split = split;
        for (int k = 0; k < 5; ++k) {
            const float v = dest.GetPixel(700, 409 + distances[k]).g;
            if (previous[k] > 0.0f) worst = std::max(worst, std::fabs(v / previous[k] - 1.0f));
            previous[k] = v;
        }
    }
    Check(splits_seen >= 2, "the sweep crosses a change of tier");
    // Measured 1.2%, all of it the light a larger radius adds.
    Check(worst < 0.03f, "the core stays continuous as the radius animates");
}

// After Effects blends 8 and 16 bpc layers in their encoded space, so a pixel
// over black shows its premultiplied value as it stands. The glow is light:
// over black it has to show Encode(light). Built as Encode(light / coverage)
// times coverage instead, a glow spilling into a transparent layer showed a
// third of its light at the edge and a tenth of it in the tail.
void TestGlowIsLightOverBlack() {
    MallocAllocator allocator;
    ThreadPoolRunner runner(4);

    const int width = 400;
    const int height = 300;
    TestImage linear_source(width, height, PixelDepth::kFloat32);
    TestImage encoded_source(width, height, PixelDepth::kBits8);
    for (int y = 130; y < 170; ++y) {
        for (int x = 180; x < 220; ++x) {
            linear_source.SetPixel(x, y, PixelF{1.0f, 1.0f, 1.0f, 1.0f});
            encoded_source.SetPixel(x, y, PixelF{1.0f, 1.0f, 1.0f, 1.0f});
        }
    }

    for (GlowModel model : {GlowModel::kClassic, GlowModel::kInverseSquare}) {
        GlowSettings settings = DefaultSettings();
        settings.model = model;
        settings.threshold = 0.0f;
        settings.radius_x = settings.radius_y = 120.0f;
        settings.dither = false;

        // The reference: the light itself, in linear float.
        settings.composite = CompositeMode::kGlowOnly;
        settings.working_space = WorkingSpace::kLinear;
        TestImage light(width, height, PixelDepth::kFloat32);
        GlowRender reference;
        reference.source = linear_source.View();
        reference.dest = light.View();
        Check(abglow::RenderGlow(settings, reference, allocator, runner) == GlowResult::kOk,
              "light reference renders");

        settings.composite = CompositeMode::kAdd;
        settings.working_space = WorkingSpace::kSrgb;
        TestImage dest(width, height, PixelDepth::kBits8);
        GlowRender render;
        render.source = encoded_source.View();
        render.dest = dest.View();
        Check(abglow::RenderGlow(settings, render, allocator, runner) == GlowResult::kOk, "8 bpc glow renders");

        const abglow::TransferFunction& srgb = abglow::TransferFunction::Srgb();
        float worst = 0.0f;
        bool premultiplied = true;
        for (int x = 222; x < 330; x += 3) {
            const float expected = light.GetPixel(x, 150).g;
            if (expected < 0.01f) continue;
            const PixelF p = dest.GetPixel(x, 150);
            worst = std::max(worst, std::fabs(srgb.Decode(p.g) / expected - 1.0f));
            if (p.g > p.a + 1.0f / 255.0f) premultiplied = false;
        }
        Check(worst < 0.06f, std::string("over black the glow shows its own light") + ModelName(model));
        Check(premultiplied, std::string("the glow is a valid premultiplied pixel") + ModelName(model));
    }
}

// Burn to White: the more a colour is overexposed the whiter it gets, which is
// the hot core of a light. Light the output can show keeps its colour, and
// float output keeps its HDR value.
void TestBurnToWhite() {
    MallocAllocator allocator;
    ThreadPoolRunner runner(4);

    const int width = 500;
    const int height = 400;
    TestImage source(width, height, PixelDepth::kFloat32);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            if (std::hypot(x - 250.0, y - 200.0) < 40.0) source.SetPixel(x, y, PixelF{1.0f, 0.0f, 0.8f, 1.0f});
        }
    }

    auto render_with = [&](abglow::HighlightRolloff mode, PixelDepth depth, PixelF* core, PixelF* tail) {
        GlowSettings settings = DefaultSettings();
        settings.model = GlowModel::kInverseSquare;
        settings.falloff = 2.0f;
        settings.threshold = 0.3f;
        settings.radius_x = settings.radius_y = 150.0f;
        settings.intensity = 3.0f;
        settings.rolloff = mode;
        TestImage dest(width, height, depth);
        GlowRender render;
        render.source = source.View();
        render.dest = dest.View();
        Check(abglow::RenderGlow(settings, render, allocator, runner) == GlowResult::kOk, "burn render succeeds");
        *core = dest.GetPixel(250, 200);
        *tail = dest.GetPixel(250, 330);
    };

    PixelF core, tail, hue_core, hue_tail;
    render_with(abglow::HighlightRolloff::kBurnToWhite, PixelDepth::kBits8, &core, &tail);
    render_with(abglow::HighlightRolloff::kPreserveHue, PixelDepth::kBits8, &hue_core, &hue_tail);
    Check(core.r > 0.8f * core.b, "an overexposed cyan burns to white");
    Check(hue_core.r < 0.01f, "preserve hue keeps it cyan");
    CheckNear(tail.r, hue_tail.r, 1.0f / 255.0f, "light the output can show keeps its colour");
    CheckNear(tail.b, hue_tail.b, 1.0f / 255.0f, "burning leaves the tail's brightness alone");

    render_with(abglow::HighlightRolloff::kBurnToWhite, PixelDepth::kFloat32, &core, &tail);
    Check(core.b > 1.0f, "float output keeps its HDR value");
    Check(core.r > 0.8f * core.b, "float output burns too");
}

// After Effects hands effects straight pixels: an anti-aliased edge keeps its
// full colour and says how much of the pixel it covers in alpha. Read as if it
// were premultiplied, that colour was divided by the coverage again - a pixel a
// tenth covered emitted as if it were ten times as bright - and the output was
// written premultiplied into a buffer the host reads as straight. The edges
// came out hard and stair-stepped with the glow on, the glow along them was a
// ragged fringe that crawled as the layer moved, and it was twice as strong at
// Third resolution as at Full. The straight path has to give exactly what the
// premultiplied one does, pixel for pixel, as the host will show it.
void TestStraightAlphaMatchesPremultiplied() {
    MallocAllocator allocator;
    ThreadPoolRunner runner(4);

    const int width = 360;
    const int height = 240;
    TestImage straight_source(width, height, PixelDepth::kBits8, AlphaMode::kStraight);
    TestImage premultiplied_source(width, height, PixelDepth::kBits8, AlphaMode::kPremultiplied);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            // An anti-aliased disc, a translucent block and a gradient edge.
            const float disc = std::clamp(40.5f - static_cast<float>(std::hypot(x - 120.3, y - 118.7)), 0.0f, 1.0f);
            PixelF colour{disc, 1.0f, 0.55f, 0.2f};
            if (x >= 220 && x < 300 && y >= 60 && y < 180) {
                colour = PixelF{0.4f + 0.004f * static_cast<float>(x - 220), 0.3f, 0.8f, 1.0f};
            }
            straight_source.SetPixel(x, y, colour);
            premultiplied_source.SetPixel(
                x, y, PixelF{colour.a, colour.r * colour.a, colour.g * colour.a, colour.b * colour.a});
        }
    }

    for (GlowModel model : {GlowModel::kClassic, GlowModel::kInverseSquare}) {
        for (CompositeMode mode : {CompositeMode::kAdd, CompositeMode::kGlowOnly}) {
            GlowSettings settings = DefaultSettings();
            settings.model = model;
            settings.working_space = WorkingSpace::kSrgb;
            settings.threshold = 0.2f;
            settings.radius_x = settings.radius_y = 90.0f;
            settings.composite = mode;
            settings.dither = false;

            auto render_with = [&](TestImage& source, AlphaMode alpha) {
                TestImage dest(width, height, PixelDepth::kBits8, alpha);
                GlowRender render;
                render.source = source.View();
                render.dest = dest.View();
                Check(abglow::RenderGlow(settings, render, allocator, runner) == GlowResult::kOk,
                      "alpha convention render succeeds");
                return dest;
            };
            const TestImage straight = render_with(straight_source, AlphaMode::kStraight);
            const TestImage premultiplied = render_with(premultiplied_source, AlphaMode::kPremultiplied);

            float worst = 0.0f;
            for (int y = 0; y < height; ++y) {
                for (int x = 0; x < width; ++x) {
                    const PixelF a = straight.GetPixel(x, y);
                    const PixelF b = premultiplied.GetPixel(x, y);
                    worst = std::max(worst, std::fabs(a.a - b.a));
                    worst = std::max(worst, std::fabs(a.r * a.a - b.r));
                    worst = std::max(worst, std::fabs(a.g * a.a - b.g));
                    worst = std::max(worst, std::fabs(a.b * a.a - b.b));
                }
            }
            Check(worst <= 2.5f / 255.0f,
                  std::string("straight pixels composite exactly as premultiplied ones do") + ModelName(model));
        }
    }
}

// With the glow on, an anti-aliased edge has to stay anti-aliased: the
// coverage of a half-covered pixel is still a half, not raised to cover its
// full colour, which is what made text look pixelated.
void TestStraightEdgesStayAntiAliased() {
    MallocAllocator allocator;
    ThreadPoolRunner runner(4);

    const int width = 300;
    const int height = 300;
    TestImage source(width, height, PixelDepth::kBits8, AlphaMode::kStraight);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const float coverage =
                std::clamp(60.5f - static_cast<float>(std::hypot(x - 150.3, y - 149.6)), 0.0f, 1.0f);
            source.SetPixel(x, y, PixelF{coverage, 0.39f, 0.77f, 0.98f});
        }
    }

    GlowSettings settings = DefaultSettings();
    settings.model = GlowModel::kInverseSquare;
    settings.working_space = WorkingSpace::kSrgb;
    settings.radius_x = settings.radius_y = 150.0f;
    settings.intensity = 0.005f;
    settings.dither = false;

    TestImage dest(width, height, PixelDepth::kBits8, AlphaMode::kStraight);
    GlowRender render;
    render.source = source.View();
    render.dest = dest.View();
    Check(abglow::RenderGlow(settings, render, allocator, runner) == GlowResult::kOk, "edge render succeeds");

    int edges = 0;
    float worst = 0.0f;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const float coverage = source.GetPixel(x, y).a;
            if (coverage < 0.15f || coverage > 0.85f) continue;
            ++edges;
            worst = std::max(worst, dest.GetPixel(x, y).a - coverage);
        }
    }
    Check(edges > 100, "the disc has an anti-aliased edge to check");
    // The faint glow adds a little coverage; reading straight as premultiplied
    // raised every one of these to the brightness of the full colour, 0.98.
    Check(worst < 0.05f, "a faint glow leaves the edge's coverage where it was");
}

// A moving shape must not make its glow flicker. The light a layer emits is
// linear in its coverage, so the total cannot change as the shape slides
// across the pixel grid, and the glow a frame later has to be the glow of the
// frame before, moved. Reading straight pixels as premultiplied swung the
// total by a fifth between quarter-pixel steps.
void TestGlowDoesNotFlicker() {
    MallocAllocator allocator;
    ThreadPoolRunner runner(4);

    const int width = 500;
    const int height = 400;
    const double step = 0.125;
    for (GlowModel model : {GlowModel::kClassic, GlowModel::kInverseSquare}) {
        GlowSettings settings = DefaultSettings();
        settings.model = model;
        settings.falloff = 2.0f;
        settings.threshold = 0.5f;
        settings.radius_x = settings.radius_y = 200.0f;
        settings.composite = CompositeMode::kGlowOnly;

        std::vector<TestImage> frames;
        std::vector<double> totals;
        for (int f = 0; f <= 8; ++f) {
            const double shift = f * step;
            TestImage source(width, height, PixelDepth::kFloat32, AlphaMode::kStraight);
            for (int y = 0; y < height; ++y) {
                for (int x = 0; x < width; ++x) {
                    // A thin ring, its coverage integrated over each pixel.
                    double covered = 0.0;
                    for (int sy = 0; sy < 4; ++sy) {
                        for (int sx = 0; sx < 4; ++sx) {
                            const double px = x + (sx + 0.5) / 4.0 - shift;
                            const double py = y + (sy + 0.5) / 4.0;
                            const double d = std::fabs(std::hypot(px - 250.0, py - 200.0) - 90.0);
                            covered += d < 2.5 ? 1.0 : 0.0;
                        }
                    }
                    const float coverage = static_cast<float>(covered / 16.0);
                    if (coverage > 0.0f) source.SetPixel(x, y, PixelF{coverage, 1.0f, 0.75f, 1.0f});
                }
            }
            frames.emplace_back(width, height, PixelDepth::kFloat32);
            GlowRender render;
            render.source = source.View();
            render.dest = frames.back().View();
            Check(abglow::RenderGlow(settings, render, allocator, runner) == GlowResult::kOk,
                  "flicker render succeeds");
            double total = 0.0;
            for (int y = 0; y < height; ++y) {
                for (int x = 0; x < width; ++x) total += frames.back().GetPixel(x, y).g;
            }
            totals.push_back(total);
        }

        const double lowest = *std::min_element(totals.begin(), totals.end());
        const double highest = *std::max_element(totals.begin(), totals.end());
        Check(highest <= lowest * 1.001,
              std::string("the glow's total light holds still as the shape moves") + ModelName(model));

        // Away from the ring itself, each frame against the one before moved by
        // the step. Measured 0.2%; reading straight as premultiplied, 55%.
        float worst = 0.0f;
        for (std::size_t f = 1; f < frames.size(); ++f) {
            std::vector<float> residuals;
            for (int y = 60; y < height - 60; y += 3) {
                for (int x = 60; x < width - 60; x += 3) {
                    const double ring = std::fabs(std::hypot(x - 250.0, y - 200.0) - 90.0);
                    if (ring < 8.0) continue;
                    const float before = frames[f - 1].GetPixel(x, y).g;
                    const float left = frames[f - 1].GetPixel(x - 1, y).g;
                    const float predicted = static_cast<float>((1.0 - step) * before + step * left);
                    const float now = frames[f].GetPixel(x, y).g;
                    residuals.push_back(std::fabs(now - predicted) / (predicted + 1e-3f));
                }
            }
            std::sort(residuals.begin(), residuals.end());
            worst = std::max(worst, residuals[residuals.size() * 99 / 100]);
        }
        Check(worst < 0.01f, std::string("the glow moves with the shape instead of flickering") + ModelName(model));
    }
}

// Every rung is upsampled into the one below it before the final
// reconstruction. Bilinear left a kink at each coarse sample, and in a strong,
// wide glow those kinks line up into faint concentric rings; the collapse is a
// quadratic B-spline now. Checked on the curvature of the log profile out of a
// small disc: a power law has a smooth n / r^2, and kinks are spikes on it.
void TestNoRingsInTheHalo() {
    MallocAllocator allocator;
    ThreadPoolRunner runner(4);

    const int size = 1200;
    TestImage source(size, size, PixelDepth::kFloat32);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const float coverage =
                std::clamp(8.5f - static_cast<float>(std::hypot(x - 600.3, y - 600.6)), 0.0f, 1.0f);
            if (coverage > 0.0f) source.SetPixel(x, y, PixelF{coverage, 4.0f * coverage, 4.0f * coverage, 4.0f * coverage});
        }
    }
    for (GlowModel model : {GlowModel::kClassic, GlowModel::kInverseSquare}) {
        GlowSettings settings = DefaultSettings();
        settings.model = model;
        settings.falloff = 2.0f;
        settings.threshold = 0.0f;
        settings.radius_x = settings.radius_y = 1200.0f;
        settings.composite = CompositeMode::kGlowOnly;
        TestImage dest(size, size, PixelDepth::kFloat32);
        GlowRender render;
        render.source = source.View();
        render.dest = dest.View();
        Check(abglow::RenderGlow(settings, render, allocator, runner) == GlowResult::kOk, "ring render succeeds");

        double spikes = 0.0;
        int angles = 0;
        for (double angle = 0.0; angle <= 1.5708; angle += 0.2618) {
            std::vector<double> curvature;
            auto sample = [&](double r) {
                const double x = 600.3 + r * std::cos(angle);
                const double y = 600.6 + r * std::sin(angle);
                const int x0 = static_cast<int>(std::floor(x));
                const int y0 = static_cast<int>(std::floor(y));
                const double fx = x - x0;
                const double fy = y - y0;
                return dest.GetPixel(x0, y0).g * (1 - fx) * (1 - fy) + dest.GetPixel(x0 + 1, y0).g * fx * (1 - fy) +
                       dest.GetPixel(x0, y0 + 1).g * (1 - fx) * fy + dest.GetPixel(x0 + 1, y0 + 1).g * fx * fy;
            };
            for (double r = 21.0; r < 560.0; r += 1.0) {
                const double a = std::log(sample(r - 1.0));
                const double b = std::log(sample(r));
                const double c = std::log(sample(r + 1.0));
                curvature.push_back(std::fabs(a - 2.0 * b + c) * r * r);
            }
            std::vector<double> sorted = curvature;
            std::sort(sorted.begin(), sorted.end());
            spikes += sorted[sorted.size() * 99 / 100] / sorted[sorted.size() / 2];
            ++angles;
        }
        // Measured 2.1 for Inverse Square; bilinear with the old per-level
        // blur scored 4.7.
        Check(spikes / angles < 3.0, std::string("the halo has no rings") + ModelName(model));
    }
}

// A near-invisible layer - a soft light at 1-3/255 opacity inside a precomp,
// stored straight in a saturated colour - must not glow as if it were bright.
// Judged on its own colour it passed the threshold, and the 1/255 steps of its
// alpha became a wide disc of posterised rings, coloured by whatever the
// invisible pixels stored. An edge next to a covered pixel still counts fully.
void TestFaintLayerDoesNotGlow() {
    MallocAllocator allocator;
    ThreadPoolRunner runner(4);

    const int width = 600;
    const int height = 400;
    TestImage source(width, height, PixelDepth::kBits8, AlphaMode::kStraight);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const double d = std::hypot(x - 300.0, y - 200.0);
            const float alpha = std::round(static_cast<float>(std::max(0.0, 1.0 - d / 250.0)) * 3.0f) / 255.0f;
            source.SetPixel(x, y, PixelF{alpha, 0.45f, 0.1f, 1.0f});
        }
    }
    GlowSettings settings = DefaultSettings();
    settings.model = GlowModel::kInverseSquare;
    settings.working_space = WorkingSpace::kSrgb;
    settings.threshold = 0.5f;
    settings.radius_x = settings.radius_y = 80.0f;
    settings.intensity = 0.79f;
    settings.exposure = 0.92f;
    settings.composite = CompositeMode::kGlowOnly;
    settings.dither = false;

    TestImage dest(width, height, PixelDepth::kBits8);
    GlowRender render;
    render.source = source.View();
    render.dest = dest.View();
    Check(abglow::RenderGlow(settings, render, allocator, runner) == GlowResult::kOk, "faint render succeeds");
    float brightest = 0.0f;
    for (int y = 0; y < height; y += 4) {
        for (int x = 0; x < width; x += 4) brightest = std::max(brightest, dest.GetPixel(x, y).b);
    }
    // Measured 0 now; judged on its own colour the disc reached 36/255.
    Check(brightest < 2.0f / 255.0f, "a near-invisible layer does not glow");
}

// Core Radius and Core Intensity set the soft rim hugging the source apart
// from the halo Radius sets. At 100% nothing changes - a project saved before
// the controls existed must open looking the same - and turning the core down
// takes the rim away without touching the halo far out.
void TestCoreIsSetApartFromTheHalo() {
    MallocAllocator allocator;
    ThreadPoolRunner runner(4);

    const int width = 900;
    const int height = 600;
    TestImage source(width, height, PixelDepth::kFloat32);
    for (int y = 250; y < 350; ++y) {
        for (int x = 400; x < 500; ++x) source.SetPixel(x, y, PixelF{1.0f, 1.0f, 0.3f, 1.0f});
    }
    for (GlowModel model : {GlowModel::kClassic, GlowModel::kInverseSquare}) {
        auto render_with = [&](float intensity, bool touch, float core_radius = 20.0f) {
            GlowSettings settings = DefaultSettings();
            settings.model = model;
            settings.falloff = 2.0f;
            settings.threshold = 0.0f;
            settings.radius_x = settings.radius_y = 400.0f;
            settings.composite = CompositeMode::kGlowOnly;
            if (touch) {
                settings.core_radius = core_radius;
                settings.core_intensity = intensity;
            }
            TestImage dest(width, height, PixelDepth::kFloat32);
            GlowRender render;
            render.source = source.View();
            render.dest = dest.View();
            Check(abglow::RenderGlow(settings, render, allocator, runner) == GlowResult::kOk, "core render succeeds");
            return dest;
        };
        const TestImage untouched = render_with(1.0f, false);
        const TestImage unity = render_with(1.0f, true);
        const TestImage none = render_with(0.0f, true);
        const TestImage hot = render_with(3.0f, true);

        bool identical = true;
        for (int y = 0; y < height; y += 7) {
            for (int x = 0; x < width; x += 7) {
                if (untouched.GetPixel(x, y).g != unity.GetPixel(x, y).g) identical = false;
            }
        }
        Check(identical, std::string("core intensity 100% leaves the glow exactly as it was") + ModelName(model));

        const float rim = untouched.GetPixel(503, 300).g;
        const float halo = untouched.GetPixel(700, 300).g;
        // Classic's finest octave is a fraction of the radius, so at 400 it
        // has hardly any core to turn; the rim is Inverse Square's.
        if (model == GlowModel::kInverseSquare) {
            Check(none.GetPixel(503, 300).g < rim * 0.8f, "turning the core down takes the rim away");
            Check(hot.GetPixel(503, 300).g > rim * 1.2f, "turning the core up brightens the rim");
        }
        CheckNear(none.GetPixel(700, 300).g, halo, halo * 0.05f,
                  std::string("the halo far out is left alone") + ModelName(model));

        // Core Radius moves the rim's light at any intensity, 100% included:
        // wider pulls it off the edge and out, tighter pulls it in, and the
        // total stays the same.
        if (model == GlowModel::kInverseSquare) {
            const TestImage wide = render_with(1.0f, true, 80.0f);
            const TestImage tight = render_with(1.0f, true, 5.0f);
            // Measured 6 px out: 0.179 at 20 px, 0.222 at 80, 0.153 at 5 -
            // a tight core keeps its light on the shape itself.
            Check(wide.GetPixel(505, 300).g > untouched.GetPixel(505, 300).g * 1.15f,
                  "a wider core radius makes a wider rim");
            Check(tight.GetPixel(505, 300).g < untouched.GetPixel(505, 300).g * 0.9f,
                  "a tighter core radius makes a tighter rim");
            Check(wide.GetPixel(540, 300).g > untouched.GetPixel(540, 300).g * 1.05f,
                  "a wider core radius carries the rim further out");
            Check(TotalEnergy(wide) > TotalEnergy(untouched) * 0.98f &&
                      TotalEnergy(wide) < TotalEnergy(untouched) * 1.02f,
                  "moving the core does not change how much light it carries");
        }
    }
}


void TestCoreSoftness() {
    MallocAllocator allocator;
    ThreadPoolRunner runner(4);

    const int width = 900;
    const int height = 600;
    TestImage source(width, height, PixelDepth::kFloat32);
    for (int y = 250; y < 350; ++y) {
        for (int x = 400; x < 500; ++x) source.SetPixel(x, y, PixelF{1.0f, 1.0f, 0.3f, 1.0f});
    }
    auto render_with = [&](float softness) {
        GlowSettings settings = DefaultSettings();
        settings.model = GlowModel::kInverseSquare;
        settings.falloff = 2.0f;
        settings.threshold = 0.0f;
        settings.radius_x = settings.radius_y = 400.0f;
        settings.composite = CompositeMode::kGlowOnly;
        settings.core_intensity = 3.0f;
        settings.core_radius = 30.0f;
        settings.core_softness = softness;
        TestImage dest(width, height, PixelDepth::kFloat32);
        GlowRender render;
        render.source = source.View();
        render.dest = dest.View();
        Check(abglow::RenderGlow(settings, render, allocator, runner) == GlowResult::kOk, "softness render succeeds");
        return dest;
    };
    const TestImage hard = render_with(0.0f);
    const TestImage soft = render_with(1.0f);
    // Measured: 6 px out 0.036 -> 0.077, 100 px out 0.0083 -> 0.0141, the
    // total unchanged. The light comes off the shape's own face, which is
    // what the hard line along the edge was made of.
    Check(soft.GetPixel(440, 300).g < hard.GetPixel(440, 300).g, "a soft core takes light off the shape's face");
    Check(soft.GetPixel(540, 300).g > hard.GetPixel(540, 300).g * 1.5f, "a soft core trails further out");
    Check(soft.GetPixel(600, 300).g > hard.GetPixel(600, 300).g * 1.3f, "a soft core fades into the halo");
    Check(TotalEnergy(soft) > TotalEnergy(hard) * 0.98f && TotalEnergy(soft) < TotalEnergy(hard) * 1.02f,
          "softening the core does not change how much light it carries");
}

void TestAspectRatio() {
    MallocAllocator allocator;
    ThreadPoolRunner runner(4);

    const int width = 900;
    const int height = 900;
    TestImage source(width, height, PixelDepth::kFloat32);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const bool inside = std::hypot(x - 450.0, y - 450.0) < 5.0;
            source.SetPixel(x, y, inside ? PixelF{1.0f, 3.0f, 3.0f, 3.0f} : PixelF{0.0f, 0.0f, 0.0f, 0.0f});
        }
    }
    for (GlowModel model : {GlowModel::kClassic, GlowModel::kInverseSquare}) {
        for (Quality quality : {Quality::kDraft, Quality::kBest}) {
            auto render_with = [&](float aspect) {
                GlowSettings settings = DefaultSettings();
                settings.model = model;
                settings.quality = quality;
                settings.threshold = 0.2f;
                settings.radius_x = settings.radius_y = 300.0f;
                settings.aspect_ratio = aspect;
                settings.composite = CompositeMode::kGlowOnly;
                TestImage dest(width, height, PixelDepth::kFloat32);
                GlowRender render;
                render.source = source.View();
                render.dest = dest.View();
                Check(abglow::RenderGlow(settings, render, allocator, runner) == GlowResult::kOk,
                      "aspect render succeeds");
                return dest;
            };
            auto extent = [&](const TestImage& image, int dx, int dy) {
                const float peak = image.GetPixel(450, 450).g;
                for (int i = 1; i < 450; ++i) {
                    if (image.GetPixel(450 + dx * i, 450 + dy * i).g < peak * 0.01f) return i;
                }
                return 450;
            };
            const TestImage round = render_with(1.0f);
            const TestImage wide = render_with(2.0f);
            const TestImage tall = render_with(0.5f);
            const TestImage streak = render_with(0.0f);
            const std::string where = std::string(ModelName(model)) +
                                      (quality == Quality::kDraft ? " (draft)" : " (best)");
            CheckNear(static_cast<float>(extent(round, 1, 0)), static_cast<float>(extent(round, 0, 1)), 1.0f,
                      "aspect 1 is round" + where);
            const float wide_ratio = static_cast<float>(extent(wide, 1, 0)) / static_cast<float>(extent(wide, 0, 1));
            const float tall_ratio = static_cast<float>(extent(tall, 0, 1)) / static_cast<float>(extent(tall, 1, 0));
            Check(wide_ratio > 1.8f && wide_ratio < 2.2f, "aspect 2 is twice as wide as tall" + where);
            Check(tall_ratio > 1.8f && tall_ratio < 2.2f, "aspect 0.5 is twice as tall as wide" + where);
            Check(extent(streak, 1, 0) <= 9, "aspect 0 does not spread sideways" + where);
            Check(extent(streak, 0, 1) >= extent(round, 0, 1), "aspect 0 is a vertical streak" + where);
            Check(TotalEnergy(wide) > TotalEnergy(round) * 0.98f && TotalEnergy(wide) < TotalEnergy(round) * 1.03f &&
                      TotalEnergy(streak) > TotalEnergy(round) * 0.98f &&
                      TotalEnergy(streak) < TotalEnergy(round) * 1.03f,
                  "squeezing the glow does not change how much light it carries" + where);
        }
    }
}

}  // namespace

int main() {
    TestPlanSanity();
    TestGaussianKernelNormalised();
    TestTransferRoundTrip();
    TestEnergyConservation();
    TestRadialFalloff();
    TestNoUpsampleCreases();
    for (GlowModel model : {GlowModel::kClassic, GlowModel::kInverseSquare}) {
        TestGlowDoesNotSlideWithRadius(model);
        TestSizeIsResolutionIndependent(model);
        TestBrightnessIsResolutionIndependent(model, AlphaMode::kPremultiplied);
        TestBrightnessIsResolutionIndependent(model, AlphaMode::kStraight);
        TestRegionOfInterestMatchesFullFrame(model);
        // A squeezed axis keeps its own grid, which must be anchored as
        // firmly as the round one.
        TestRegionOfInterestMatchesFullFrame(model, 0.4f);
        TestRegionOfInterestMatchesFullFrame(model, 2.5f);
        TestGlowTracksSubPixelMotion(model);
    }
    TestDitherIsQuietAndStill();
    TestHighlightRolloffKeepsHue();
    TestGlowIsBloomNotBlur();
    TestSmallAndLargeShapesGlowAlike();
    TestScreenStaysPositive();
    TestScreenReachesItsCeilingSmoothly();
    TestFalloffFollowsThePowerLaw();
    TestGlowHasALongTailWithoutHaze();
    TestGlowAberrationSpreadsTheChannels();
    TestSourceOpacityLeavesTheGlowAlone();
    TestUnmultReadsCoverageFromBrightness();
    TestSaturationBiasFavoursColour();
    TestAnisotropicRadius();
    TestAspectRatio();
    TestCoreSoftness();
    TestThresholdAndPassThrough();
    TestTransparentInput();
    TestHdrNotClamped();
    TestDepthConsistency();
    TestExpandedBounds();
    TestOffsetPassThrough();
    TestEmptySourceAndBadArguments();
    TestZeroRadiusAndZeroIntensity();
    TestInverseSquarePlan();
    TestInverseSquareFollowsThePowerLaw();
    TestRadiusExtendsTheReachNotTheCore();
    TestInverseSquareDoesNotPop();
    TestGlowIsLightOverBlack();
    TestBurnToWhite();
    TestStraightAlphaMatchesPremultiplied();
    TestStraightEdgesStayAntiAliased();
    TestGlowDoesNotFlicker();
    TestNoRingsInTheHalo();
    TestFaintLayerDoesNotGlow();
    TestCoreIsSetApartFromTheHalo();

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
