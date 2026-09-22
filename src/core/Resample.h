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

void ScaleInPlace(ImageF& image, const PixelF& scale, TaskRunner& runner);

}  // namespace abglow
