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
    for (float radius : {0.5f, 2.0f, 8.0f, 40.0f, 200.0f, 1000.0f}) {
        const float sigma = abglow::RadiusToSigma(radius);
        const abglow::GlowPlan plan = abglow::MakeGlowPlan(sigma, Quality::kNormal);
        float sum = 0.0f;
        for (int i = 0; i < plan.level_count; ++i) sum += plan.weights[i];
        CheckNear(sum, 1.0f, 1e-4f, "plan weights normalise at radius " + std::to_string(radius));
        Check(plan.level_count >= 1 && plan.level_count <= abglow::kMaxPyramidLevels, "plan level count in range");
        Check(plan.base_scale >= 1 && plan.base_scale <= 8, "plan base scale in range");
    }

    float previous = 0.0f;
    for (float radius = 1.0f; radius <= 400.0f; radius *= 1.3f) {
        const abglow::GlowPlan plan = abglow::MakeGlowPlan(abglow::RadiusToSigma(radius), Quality::kNormal);
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
}

}  // namespace

int main() {
    TestPlanSanity();
    TestGaussianKernelNormalised();
    TestTransferRoundTrip();
    TestEnergyConservation();
    TestRadialFalloff();
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
