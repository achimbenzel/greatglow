#include "Resample.h"

#include <algorithm>
#include <cmath>

namespace abglow {
namespace {

inline int ClampInt(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

inline void AccumulateScaled(PixelF& dst, const PixelF& src, float w) {
    dst.a += src.a * w;
    dst.r += src.r * w;
    dst.g += src.g * w;
    dst.b += src.b * w;
}

}  // namespace

// [1 3 3 1]/8 is the tent over two cells. A 2x2 box carries the light but not
// its centre of mass, so content crossing the grid makes each octave lead and
// lag; with a deep ladder those errors accumulate into visible wander.
void DownsampleHalf(const ImageF& src, ImageF& dst, TaskRunner& runner) {
    if (src.Empty() || dst.Empty()) return;

    static constexpr float kTent[4] = {0.125f, 0.375f, 0.375f, 0.125f};

    ParallelRows(runner, dst.height, [&](int begin, int end, int) {
        for (int y = begin; y < end; ++y) {
            const PixelF* rows[4];
            for (int j = 0; j < 4; ++j) rows[j] = src.Row(ClampInt(y * 2 - 1 + j, 0, src.height - 1));
            PixelF* out = dst.Row(y);
            for (int x = 0; x < dst.width; ++x) {
                int sx[4];
                for (int i = 0; i < 4; ++i) sx[i] = ClampInt(x * 2 - 1 + i, 0, src.width - 1);
                PixelF acc{0.0f, 0.0f, 0.0f, 0.0f};
                for (int j = 0; j < 4; ++j) {
                    const PixelF* row = rows[j];
                    for (int i = 0; i < 4; ++i) AccumulateScaled(acc, row[sx[i]], kTent[j] * kTent[i]);
                }
                out[x] = acc;
            }
        }
    });
}

void UpsampleHalfAccumulate(const ImageF& src, ImageF& dst, float dst_weight, TaskRunner& runner) {
    if (src.Empty() || dst.Empty()) return;

    ParallelRows(runner, dst.height, [&](int begin, int end, int) {
        for (int y = begin; y < end; ++y) {
            const float v = static_cast<float>(y) * 0.5f - 0.25f;
            const int v0 = static_cast<int>(std::floor(v));
            const float fy = v - static_cast<float>(v0);
            const int sy0 = ClampInt(v0, 0, src.height - 1);
            const int sy1 = ClampInt(v0 + 1, 0, src.height - 1);
            const PixelF* row0 = src.Row(sy0);
            const PixelF* row1 = src.Row(sy1);
            PixelF* out = dst.Row(y);
            for (int x = 0; x < dst.width; ++x) {
                const float u = static_cast<float>(x) * 0.5f - 0.25f;
                const int u0 = static_cast<int>(std::floor(u));
                const float fx = u - static_cast<float>(u0);
                const int sx0 = ClampInt(u0, 0, src.width - 1);
                const int sx1 = ClampInt(u0 + 1, 0, src.width - 1);

                const float w00 = (1.0f - fx) * (1.0f - fy);
                const float w01 = fx * (1.0f - fy);
                const float w10 = (1.0f - fx) * fy;
                const float w11 = fx * fy;

                PixelF acc = out[x];
                acc.a *= dst_weight;
                acc.r *= dst_weight;
                acc.g *= dst_weight;
                acc.b *= dst_weight;
                AccumulateScaled(acc, row0[sx0], w00);
                AccumulateScaled(acc, row0[sx1], w01);
                AccumulateScaled(acc, row1[sx0], w10);
                AccumulateScaled(acc, row1[sx1], w11);
                out[x] = acc;
            }
        }
    });
}

void ScaleInPlace(ImageF& image, float scale, TaskRunner& runner) {
    if (image.Empty()) return;
    ParallelRows(runner, image.height, [&](int begin, int end, int) {
        for (int y = begin; y < end; ++y) {
            PixelF* row = image.Row(y);
            for (int x = 0; x < image.width; ++x) {
                row[x].a *= scale;
                row[x].r *= scale;
                row[x].g *= scale;
                row[x].b *= scale;
            }
        }
    });
}

}  // namespace abglow
