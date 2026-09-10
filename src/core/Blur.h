#pragma once

#include "ImageF.h"
#include "TaskRunner.h"

namespace abglow {

constexpr int kMaxBlurTaps = 64;

struct BlurKernel {
    float weights[kMaxBlurTaps] = {};  // weights[0] is the centre tap
    int radius = 0;

    static BlurKernel Gaussian(float sigma, int max_radius = kMaxBlurTaps - 1);
};

// Separable edge-clamped blur. `temp` must be at least as large as `image`.
// Separate kernels keep the glow round when pixels are not square.
void BlurSeparable(ImageF& image, ImageF& temp, const BlurKernel& horizontal, const BlurKernel& vertical,
                   TaskRunner& runner);

}  // namespace abglow
