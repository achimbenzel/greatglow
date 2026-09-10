#pragma once

#include "ImageF.h"
#include "TaskRunner.h"

namespace abglow {

// Box-averages `src` into `dst`, which must be ceil(size / 2).
void DownsampleHalf(const ImageF& src, ImageF& dst, TaskRunner& runner);

// dst = dst * dst_weight + bilinear upsample of the half-resolution `src`.
void UpsampleHalfAccumulate(const ImageF& src, ImageF& dst, float dst_weight, TaskRunner& runner);

void ScaleInPlace(ImageF& image, float scale, TaskRunner& runner);

PixelF SampleBilinear(const ImageF& image, float u, float v);

// Cubic B-spline: smoother than bilinear and free of overshoot, which keeps
// large glows free of interpolation creases and banding.
PixelF SampleBSpline(const ImageF& image, float u, float v);

}  // namespace abglow
