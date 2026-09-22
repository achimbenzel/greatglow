// Renders the reference test scene through the glow pipeline and writes PNGs so
// the result can be judged by eye without After Effects.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include "TestSupport.h"
#include "core/GlowPipeline.h"

namespace {

using abglow::CompositeMode;
using abglow::GlowRender;
using abglow::GlowSettings;
using abglow::PixelDepth;
using abglow::PixelF;
using abglow::Quality;
using abglow::WorkingSpace;
using abglow_test::MallocAllocator;
using abglow_test::TestImage;
using abglow_test::ThreadPoolRunner;

void FillRect(TestImage& image, int x0, int y0, int x1, int y1, const PixelF& colour) {
    for (int y = y0; y < y1; ++y) {
        if (y < 0 || y >= image.View().height) continue;
        for (int x = x0; x < x1; ++x) {
            if (x < 0 || x >= image.View().width) continue;
            image.SetPixel(x, y, colour);
        }
    }
}

// Blocky 5x7 glyphs: enough to judge how the glow reads around thin strokes.
void DrawGlyph(TestImage& image, char glyph, int x, int y, int scale, const PixelF& colour) {
    static const struct {
        char c;
        const char* rows[7];
    } kFont[] = {
        {'A', {".###.", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"}},
        {'B', {"####.", "#...#", "####.", "#...#", "#...#", "#...#", "####."}},
        {'G', {".###.", "#...#", "#....", "#..##", "#...#", "#...#", ".###."}},
        {'L', {"#....", "#....", "#....", "#....", "#....", "#....", "#####"}},
        {'O', {".###.", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."}},
        {'W', {"#...#", "#...#", "#...#", "#.#.#", "#.#.#", "##.##", "#...#"}},
    };
    for (const auto& entry : kFont) {
        if (entry.c != glyph) continue;
        for (int row = 0; row < 7; ++row) {
            for (int col = 0; col < 5; ++col) {
                if (entry.rows[row][col] != '#') continue;
                FillRect(image, x + col * scale, y + row * scale, x + (col + 1) * scale, y + (row + 1) * scale,
                         colour);
            }
        }
        return;
    }
}

void DrawText(TestImage& image, const std::string& text, int x, int y, int scale, const PixelF& colour) {
    int cursor = x;
    for (char c : text) {
        DrawGlyph(image, c, cursor, y, scale, colour);
        cursor += 6 * scale;
    }
}

void BuildScene(TestImage& image) {
    const int width = image.View().width;
    const int height = image.View().height;
    FillRect(image, 0, 0, width, height, PixelF{1.0f, 0.0f, 0.0f, 0.0f});

    DrawText(image, "ABGLOW", 40, 40, 4, PixelF{1.0f, 1.0f, 1.0f, 1.0f});

    // Saturated shapes, adjacent so colour bleed is visible.
    FillRect(image, 40, 160, 120, 240, PixelF{1.0f, 1.0f, 0.05f, 0.05f});
    FillRect(image, 120, 160, 200, 240, PixelF{1.0f, 0.05f, 1.0f, 0.05f});
    FillRect(image, 200, 160, 280, 240, PixelF{1.0f, 0.05f, 0.05f, 1.0f});

    // Small bright points of increasing intensity.
    for (int i = 0; i < 6; ++i) {
        const float level = 1.0f + static_cast<float>(i) * 4.0f;
        image.SetPixel(360 + i * 30, 300, PixelF{1.0f, level, level, level});
    }

    // HDR disc.
    const int cx = 560;
    const int cy = 140;
    for (int y = cy - 40; y <= cy + 40; ++y) {
        for (int x = cx - 40; x <= cx + 40; ++x) {
            const float dx = static_cast<float>(x - cx);
            const float dy = static_cast<float>(y - cy);
            if (dx * dx + dy * dy <= 40.0f * 40.0f) {
                image.SetPixel(x, y, PixelF{1.0f, 24.0f, 20.0f, 8.0f});
            }
        }
    }

    // Thin lines and a gradient ramp to expose banding.
    for (int i = 0; i < 4; ++i) {
        FillRect(image, 340, 200 + i * 14, 700, 201 + i * 14, PixelF{1.0f, 1.0f, 1.0f, 1.0f});
    }
    for (int x = 40; x < 300; ++x) {
        const float t = static_cast<float>(x - 40) / 260.0f;
        FillRect(image, x, 300, x + 1, 360, PixelF{1.0f, t, t * 0.6f, t * 0.2f});
    }

    // Semi-transparent block: checks premultiplied handling.
    FillRect(image, 620, 280, 720, 380, PixelF{0.5f, 0.5f, 0.5f, 0.5f});
}

struct Variant {
    std::string name;
    GlowSettings settings;
};

GlowSettings Base() {
    GlowSettings s;
    s.threshold = 0.35f;
    s.threshold_softness = 0.4f;
    s.radius_x = 45.0f;
    s.radius_y = 45.0f;
    s.intensity = 1.0f;
    s.exposure = 0.0f;
    s.saturation = 1.0f;
    s.quality = Quality::kNormal;
    s.composite = CompositeMode::kAdd;
    s.working_space = WorkingSpace::kSrgb;
    return s;
}

// The plug-in's defaults: the inverse-square model at the radius a new instance
// gets.
GlowSettings InverseSquare() {
    GlowSettings s = Base();
    s.model = abglow::GlowModel::kInverseSquare;
    s.falloff = 2.0f;
    s.radius_x = s.radius_y = 150.0f;
    return s;
}

// Measures where the glow of a point source falls to a fraction of its peak,
// which is how the radius control is calibrated.
void ReportFalloff(MallocAllocator& allocator, abglow::TaskRunner& runner) {
    std::printf("\nradius  sigma   50%%    25%%    10%%     1%%\n");
    for (float radius : {10.0f, 25.0f, 50.0f, 100.0f, 200.0f, 400.0f}) {
        const int size = 1200;
        TestImage source(size, size, PixelDepth::kFloat32);
        TestImage dest(size, size, PixelDepth::kFloat32);
        source.SetPixel(size / 2, size / 2, PixelF{1.0f, 1000.0f, 1000.0f, 1000.0f});

        GlowSettings settings = Base();
        settings.threshold = 0.0f;
        settings.working_space = WorkingSpace::kLinear;
        settings.quality = Quality::kHigh;
        settings.composite = CompositeMode::kGlowOnly;
        settings.radius_x = settings.radius_y = radius;

        GlowRender render;
        render.source = source.View();
        render.dest = dest.View();
        abglow::RenderGlow(settings, render, allocator, runner);

        // Measured left-to-right and halved, so a sub-pixel shift of the glow
        // cancels instead of being read as a size change.
        float peak = 0.0f;
        for (int x = 0; x < size; ++x) peak = std::max(peak, dest.GetPixel(x, size / 2).g);
        float marks[4] = {-1.0f, -1.0f, -1.0f, -1.0f};
        const float levels[4] = {0.5f, 0.25f, 0.1f, 0.01f};
        for (int m = 0; m < 4; ++m) {
            float left = -1.0f;
            float right = -1.0f;
            for (int x = 1; x < size; ++x) {
                const float a = dest.GetPixel(x - 1, size / 2).g / peak;
                const float b = dest.GetPixel(x, size / 2).g / peak;
                if (left < 0.0f && a < levels[m] && b >= levels[m]) {
                    left = static_cast<float>(x - 1) + (levels[m] - a) / (b - a);
                }
                if (left >= 0.0f && a >= levels[m] && b < levels[m]) {
                    right = static_cast<float>(x - 1) + (a - levels[m]) / (a - b);
                }
            }
            if (left >= 0.0f && right >= 0.0f) marks[m] = (right - left) * 0.5f;
        }
        std::printf("%6.0f %6.1f %6.0f %6.0f %6.0f %6.0f\n", radius, abglow::RadiusToSigma(radius), marks[0],
                    marks[1], marks[2], marks[3]);
    }
    std::printf("\n");
}

}  // namespace

int main(int argc, char** argv) {
    const std::string out_dir = argc > 1 ? argv[1] : ".";
    const int width = 768;
    const int height = 432;

    std::vector<Variant> variants;
    {
        Variant v{"default", Base()};
        variants.push_back(v);
    }
    {
        Variant v{"radius_small", Base()};
        v.settings.radius_x = v.settings.radius_y = 8.0f;
        variants.push_back(v);
    }
    {
        Variant v{"radius_large", Base()};
        v.settings.radius_x = v.settings.radius_y = 220.0f;
        variants.push_back(v);
    }
    {
        Variant v{"radius_large_draft", Base()};
        v.settings.radius_x = v.settings.radius_y = 220.0f;
        v.settings.quality = Quality::kDraft;
        variants.push_back(v);
    }
    {
        Variant v{"radius_large_best", Base()};
        v.settings.radius_x = v.settings.radius_y = 220.0f;
        v.settings.quality = Quality::kBest;
        variants.push_back(v);
    }
    {
        Variant v{"threshold_high", Base()};
        v.settings.threshold = 0.85f;
        variants.push_back(v);
    }
    {
        Variant v{"threshold_zero", Base()};
        v.settings.threshold = 0.0f;
        variants.push_back(v);
    }
    {
        Variant v{"glow_only", Base()};
        v.settings.composite = CompositeMode::kGlowOnly;
        variants.push_back(v);
    }
    {
        Variant v{"desaturated", Base()};
        v.settings.saturation = 0.0f;
        variants.push_back(v);
    }
    {
        Variant v{"tinted", Base()};
        v.settings.saturation = 0.4f;
        v.settings.tint_r = 0.35f;
        v.settings.tint_g = 0.6f;
        v.settings.tint_b = 1.0f;
        v.settings.tint_amount = 1.0f;
        variants.push_back(v);
    }
    {
        Variant v{"quality_draft", Base()};
        v.settings.quality = Quality::kDraft;
        variants.push_back(v);
    }
    {
        Variant v{"quality_best", Base()};
        v.settings.quality = Quality::kBest;
        variants.push_back(v);
    }
    {
        Variant v{"exposure_up", Base()};
        v.settings.exposure = 2.0f;
        variants.push_back(v);
    }
    {
        Variant v{"linear_space", Base()};
        v.settings.working_space = WorkingSpace::kLinear;
        variants.push_back(v);
    }
    {
        Variant v{"inverse_square", InverseSquare()};
        variants.push_back(v);
    }
    {
        Variant v{"inverse_square_large", InverseSquare()};
        v.settings.radius_x = v.settings.radius_y = 400.0f;
        variants.push_back(v);
    }
    {
        Variant v{"inverse_square_falloff3", InverseSquare()};
        v.settings.falloff = 3.0f;
        variants.push_back(v);
    }
    {
        Variant v{"burn_to_white", InverseSquare()};
        v.settings.intensity = 2.0f;
        v.settings.rolloff = abglow::HighlightRolloff::kBurnToWhite;
        variants.push_back(v);
    }

    MallocAllocator allocator;
    ThreadPoolRunner runner(static_cast<int>(std::thread::hardware_concurrency()));

    TestImage source(width, height, PixelDepth::kFloat32);
    BuildScene(source);
    abglow_test::WritePng(out_dir + "/scene_source.png", source);

    for (const Variant& variant : variants) {
        TestImage dest(width, height, PixelDepth::kFloat32);
        GlowRender render;
        render.source = source.View();
        render.dest = dest.View();

        const auto start = std::chrono::steady_clock::now();
        const abglow::GlowResult result = abglow::RenderGlow(variant.settings, render, allocator, runner);
        const auto end = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(end - start).count();

        std::printf("%-16s %s  %6.2f ms\n", variant.name.c_str(),
                    result == abglow::GlowResult::kOk ? "ok  " : "FAIL", ms);
        abglow_test::WritePng(out_dir + "/scene_" + variant.name + ".png", dest);
    }

    // 8 bpc render of the same scene, to judge banding.
    TestImage source8(width, height, PixelDepth::kBits8);
    BuildScene(source8);
    TestImage dest8(width, height, PixelDepth::kBits8);
    GlowRender render8;
    render8.source = source8.View();
    render8.dest = dest8.View();
    GlowSettings settings8 = Base();
    settings8.radius_x = settings8.radius_y = 160.0f;
    abglow::RenderGlow(settings8, render8, allocator, runner);
    abglow_test::WritePng(out_dir + "/scene_8bpc_dither.png", dest8);

    settings8.dither = false;
    TestImage dest8_nodither(width, height, PixelDepth::kBits8);
    render8.dest = dest8_nodither.View();
    abglow::RenderGlow(settings8, render8, allocator, runner);
    abglow_test::WritePng(out_dir + "/scene_8bpc_nodither.png", dest8_nodither);

    abglow_test::WriteZoomedCrop(out_dir + "/crop_8bpc_dither.png", dest8, 90, 380, 200, 40, 3);
    abglow_test::WriteZoomedCrop(out_dir + "/crop_8bpc_nodither.png", dest8_nodither, 90, 380, 200, 40, 3);
    std::printf("banding run (dither)   = %d px\n", abglow_test::LongestFlatRun(dest8, 400));
    std::printf("banding run (no dither)= %d px\n", abglow_test::LongestFlatRun(dest8_nodither, 400));

    ReportFalloff(allocator, runner);

    std::printf("allocations=%d frees=%d\n", allocator.allocations(), allocator.frees());
    return 0;
}
