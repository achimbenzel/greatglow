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

void DownsampleHalf(const ImageF& src, ImageF& dst, TaskRunner& runner) {
    if (src.Empty() || dst.Empty()) return;

    ParallelRows(runner, dst.height, [&](int begin, int end, int) {
        for (int y = begin; y < end; ++y) {
            const int sy0 = ClampInt(y * 2, 0, src.height - 1);
            const int sy1 = ClampInt(y * 2 + 1, 0, src.height - 1);
            const PixelF* row0 = src.Row(sy0);
            const PixelF* row1 = src.Row(sy1);
            PixelF* out = dst.Row(y);
            for (int x = 0; x < dst.width; ++x) {
                const int sx0 = ClampInt(x * 2, 0, src.width - 1);
                const int sx1 = ClampInt(x * 2 + 1, 0, src.width - 1);
                const PixelF& p00 = row0[sx0];
                const PixelF& p01 = row0[sx1];
                const PixelF& p10 = row1[sx0];
                const PixelF& p11 = row1[sx1];
                out[x] = PixelF{(p00.a + p01.a + p10.a + p11.a) * 0.25f,
                                (p00.r + p01.r + p10.r + p11.r) * 0.25f,
                                (p00.g + p01.g + p10.g + p11.g) * 0.25f,
                                (p00.b + p01.b + p10.b + p11.b) * 0.25f};
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
