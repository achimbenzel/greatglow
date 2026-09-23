#pragma once

#include "ImageF.h"
#include "TaskRunner.h"

namespace abglow {

// Box-averages `src` into `dst`, which must be ceil(size / 2).
void DownsampleHalf(const ImageF& src, ImageF& dst, TaskRunner& runner);

// The same filter into a larger level: `src`'s first pixel pair lands on
// `dst` pixel (offset_x, offset_y), and the rest of `dst` is cleared. Outside
// `src` there is no light, so the filter reads zeros there instead of
// clamping.
void DownsampleHalfInto(const ImageF& src, ImageF& dst, int offset_x, int offset_y, TaskRunner& runner);

// dst = dst * dst_weight + quadratic B-spline upsample of the half-resolution
// `src`.
void UpsampleHalfAccumulate(const ImageF& src, ImageF& dst, const PixelF& dst_weight, TaskRunner& runner);

// The same two filters with each axis on its own schedule: an axis that is not
// halved (or doubled) is passed through one to one. The pyramid halves an axis
// only once its own blur can survive it, which is what lets a glow be much
// wider than it is tall. `offset_*` places src's first pixel (pair) on that dst
// pixel, as for DownsampleHalfInto; `zero_outside` reads zeros past src's edge
// instead of clamping.
void DownsampleAxes(const ImageF& src, ImageF& dst, bool half_x, bool half_y, int offset_x, int offset_y,
                    bool zero_outside, TaskRunner& runner);
void UpsampleAxesAccumulate(const ImageF& src, ImageF& dst, bool double_x, bool double_y,
                            const PixelF& dst_weight, TaskRunner& runner);

void ScaleInPlace(ImageF& image, const PixelF& scale, TaskRunner& runner);

}  // namespace abglow
