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

using abglow::CompositeMode;
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
        Check(sigma > previous, "effective sigma grows with radius");
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
    Check(profile[200] > 0.0f, "glow keeps a soft tail");
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
void TestGlowDoesNotSlideWithRadius() {
    MallocAllocator allocator;
    ThreadPoolRunner runner(4);

    const int layer_w = 233;
    const int layer_h = 79;
    float previous = -1.0f;
    float worst = 0.0f;

    for (float radius = 200.0f; radius <= 240.0f; radius += 4.0f) {
        const float sigma = abglow::RadiusToSigma(radius);
        const int expansion = static_cast<int>(std::ceil(abglow::MakeGlowPlan(sigma, Quality::kNormal, layer_w, layer_h).Reach()));
        const int dest_w = layer_w + 2 * expansion;
        const int dest_h = layer_h + 2 * expansion;

        TestImage source(layer_w, layer_h, PixelDepth::kFloat32);
        TestImage dest(dest_w, dest_h, PixelDepth::kFloat32);
        for (int y = 0; y < layer_h; ++y) {
            for (int x = 0; x < layer_w; ++x) source.SetPixel(x, y, PixelF{1.0f, 1.0f, 1.0f, 1.0f});
        }

        GlowSettings settings = DefaultSettings();
        settings.threshold = 0.0f;
        settings.radius_x = settings.radius_y = radius;
        settings.composite = CompositeMode::kGlowOnly;

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
    Check(worst < 0.25f, "glow stays put as the radius animates");
}

// The same glow, rendered at each of After Effects' resolutions, must come out
// the same size in composition space.
void TestSizeIsResolutionIndependent() {
    MallocAllocator allocator;
    ThreadPoolRunner runner(4);

    const float radius_comp = 600.0f;
    const int layer_w_comp = 320;
    const int layer_h_comp = 120;
    float widest = 0.0f;
    float narrowest = 1e9f;

    for (int den : {1, 2, 3, 4}) {
        const float radius = radius_comp / static_cast<float>(den);
        const float sigma = abglow::RadiusToSigma(radius);
        const int layer_w = layer_w_comp / den;
        const int layer_h = layer_h_comp / den;
        const int expansion = static_cast<int>(
            std::ceil(abglow::MakeGlowPlan(sigma, Quality::kNormal, layer_w, layer_h).Reach()));
        const int dest_w = layer_w + 2 * expansion;
        const int dest_h = layer_h + 2 * expansion;

        TestImage source(layer_w, layer_h, PixelDepth::kFloat32);
        TestImage dest(dest_w, dest_h, PixelDepth::kFloat32);
        for (int y = 0; y < layer_h; ++y) {
            for (int x = 0; x < layer_w; ++x) source.SetPixel(x, y, PixelF{1.0f, 1.0f, 1.0f, 1.0f});
        }

        GlowSettings settings = DefaultSettings();
        settings.threshold = 0.0f;
        settings.radius_x = settings.radius_y = radius;
        settings.composite = CompositeMode::kGlowOnly;

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
    Check(widest <= narrowest * 1.02f, "glow is the same size at every render resolution");
}

// After Effects renders a reduced-resolution preview from a downsampled layer,
// so anti-aliased edges carry the same light in fewer, partially covered
// pixels. Extraction has to stay linear in coverage or the same glow comes out
// dimmer at Half and Quarter - thresholding the premultiplied value made it 8%
// dimmer at Quarter on text.
void TestBrightnessIsResolutionIndependent() {
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
        TestImage source(width, height, PixelDepth::kFloat32);
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
                const float alpha = covered / static_cast<float>(den);
                if (alpha <= 0.0f) continue;
                source.SetPixel(x, y, PixelF{alpha, 0.0f, alpha, alpha});
            }
        }

        GlowSettings settings = DefaultSettings();
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
    Check(brightest <= dimmest * 1.02, "glow is the same brightness at every render resolution");
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
void TestRegionOfInterestMatchesFullFrame() {
    MallocAllocator allocator;
    ThreadPoolRunner runner(4);

    const int layer_w = 400;
    const int layer_h = 300;
    const float radius = 300.0f;
    const int expansion = static_cast<int>(std::ceil(
        abglow::MakeGlowPlan(abglow::RadiusToSigma(radius), Quality::kNormal, layer_w, layer_h).Reach()));

    TestImage source(layer_w, layer_h, PixelDepth::kFloat32);
    for (int y = 100; y < 200; ++y) {
        for (int x = 150; x < 250; ++x) source.SetPixel(x, y, PixelF{1.0f, 4.0f, 4.0f, 4.0f});
    }

    GlowSettings settings = DefaultSettings();
    settings.threshold = 0.5f;
    settings.radius_x = settings.radius_y = radius;
    settings.composite = CompositeMode::kGlowOnly;

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
        Check(worst < 1e-3f, "a requested window matches the same pixels of the full render");
    }
}

// Decimating the highlights with a box preserves their light but not their
// centre of mass, so a shape crossing the pyramid grid made the glow lead and
// lag by up to a twelfth of a cell - a sawtooth with the period of the pyramid
// step, which is the shimmer seen on a moving layer. A tent prefilter
// reproduces linear functions, so the centroid survives the decimation.
void TestGlowTracksSubPixelMotion() {
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
    Check(worst < 0.05f, "glow follows the source through sub-pixel motion");
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

    TestImage source(48, 48, PixelDepth::kFloat32);
    TestImage dest(48, 48, PixelDepth::kFloat32);
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

}  // namespace

int main() {
    TestPlanSanity();
    TestGaussianKernelNormalised();
    TestTransferRoundTrip();
    TestEnergyConservation();
    TestRadialFalloff();
    TestNoUpsampleCreases();
    TestGlowDoesNotSlideWithRadius();
    TestSizeIsResolutionIndependent();
    TestBrightnessIsResolutionIndependent();
    TestDitherIsQuietAndStill();
    TestRegionOfInterestMatchesFullFrame();
    TestGlowTracksSubPixelMotion();
    TestThresholdAndPassThrough();
    TestTransparentInput();
    TestHdrNotClamped();
    TestDepthConsistency();
    TestExpandedBounds();
    TestOffsetPassThrough();
    TestEmptySourceAndBadArguments();
    TestZeroRadiusAndZeroIntensity();

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
