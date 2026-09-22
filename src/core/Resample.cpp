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

void DownsampleHalfInto(const ImageF& src, ImageF& dst, int offset_x, int offset_y, TaskRunner& runner) {
    if (dst.Empty()) return;

    static constexpr float kTent[4] = {0.125f, 0.375f, 0.375f, 0.125f};

    ParallelRows(runner, dst.height, [&](int begin, int end, int) {
        for (int y = begin; y < end; ++y) {
            PixelF* out = dst.Row(y);
            for (int x = 0; x < dst.width; ++x) out[x] = PixelF{0.0f, 0.0f, 0.0f, 0.0f};
            if (src.Empty()) continue;
            const int sy = 2 * (y - offset_y) - 1;
            if (sy + 3 < 0 || sy >= src.height) continue;
            // Columns whose four taps reach into src at all.
            const int x_begin = std::max(0, offset_x - 1);
            const int x_end = std::min(dst.width, offset_x + (src.width + 2) / 2 + 1);
            for (int j = 0; j < 4; ++j) {
                const int row_index = sy + j;
                if (row_index < 0 || row_index >= src.height) continue;
                const PixelF* row = src.Row(row_index);
                for (int x = x_begin; x < x_end; ++x) {
                    const int sx = 2 * (x - offset_x) - 1;
                    PixelF acc = out[x];
                    for (int i = 0; i < 4; ++i) {
                        const int column = sx + i;
                        if (column < 0 || column >= src.width) continue;
                        AccumulateScaled(acc, row[column], kTent[j] * kTent[i]);
                    }
                    out[x] = acc;
                }
            }
        }
    });
}

// The collapse has to be as smooth as the final reconstruction. Bilinear
// leaves a kink at every source sample, and in a strong, wide glow those kinks
// stack up from one level to the next into faint concentric rings. A quadratic
// B-spline has a continuous first derivative; at the two phases a 2x upsample
// needs, it is the three taps below.
void UpsampleHalfAccumulate(const ImageF& src, ImageF& dst, const PixelF& dst_weight, TaskRunner& runner) {
    if (src.Empty() || dst.Empty()) return;

    // Output 2k sits a quarter of a source pixel before source k, 2k+1 a quarter
    // after it.
    static constexpr float kEven[3] = {0.28125f, 0.6875f, 0.03125f};
    static constexpr float kOdd[3] = {0.03125f, 0.6875f, 0.28125f};

    ParallelRows(runner, dst.height, [&](int begin, int end, int) {
        for (int y = begin; y < end; ++y) {
            const int cy = y >> 1;
            const float* wy = (y & 1) ? kOdd : kEven;
            const PixelF* rows[3];
            for (int j = 0; j < 3; ++j) rows[j] = src.Row(ClampInt(cy - 1 + j, 0, src.height - 1));
            PixelF* out = dst.Row(y);
            for (int x = 0; x < dst.width; ++x) {
                const int cx = x >> 1;
                const float* wx = (x & 1) ? kOdd : kEven;
                int sx[3];
                for (int i = 0; i < 3; ++i) sx[i] = ClampInt(cx - 1 + i, 0, src.width - 1);

                PixelF acc = out[x];
                acc.a *= dst_weight.a;
                acc.r *= dst_weight.r;
                acc.g *= dst_weight.g;
                acc.b *= dst_weight.b;
                for (int j = 0; j < 3; ++j) {
                    const PixelF* row = rows[j];
                    for (int i = 0; i < 3; ++i) AccumulateScaled(acc, row[sx[i]], wy[j] * wx[i]);
                }
                out[x] = acc;
            }
        }
    });
}

void ScaleInPlace(ImageF& image, const PixelF& scale, TaskRunner& runner) {
    if (image.Empty()) return;
    ParallelRows(runner, image.height, [&](int begin, int end, int) {
        for (int y = begin; y < end; ++y) {
            PixelF* row = image.Row(y);
            for (int x = 0; x < image.width; ++x) {
                row[x].a *= scale.a;
                row[x].r *= scale.r;
                row[x].g *= scale.g;
                row[x].b *= scale.b;
            }
        }
    });
}

}  // namespace abglow
