#include "Blur.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace abglow {
namespace {

// The blur runs over the four channels of a pixel as one contiguous vector,
// which is what lets the compiler emit SIMD for the tap loops.
constexpr int kChannels = 4;

void BlurRowsHorizontal(const ImageF& src, ImageF& dst, const BlurKernel& kernel, int y_begin, int y_end) {
    const int radius = kernel.radius;
    const int width = src.width;
    const int interior_begin = std::min(radius, width);
    const int interior_end = std::max(width - radius, interior_begin);

    for (int y = y_begin; y < y_end; ++y) {
        const float* src_row = reinterpret_cast<const float*>(src.Row(y));
        float* dst_row = reinterpret_cast<float*>(dst.Row(y));

        auto blur_clamped = [&](int x_begin, int x_end) {
            for (int x = x_begin; x < x_end; ++x) {
                float acc[kChannels];
                for (int c = 0; c < kChannels; ++c) acc[c] = src_row[x * kChannels + c] * kernel.weights[0];
                for (int t = 1; t <= radius; ++t) {
                    const int xl = std::max(x - t, 0) * kChannels;
                    const int xr = std::min(x + t, width - 1) * kChannels;
                    const float w = kernel.weights[t];
                    for (int c = 0; c < kChannels; ++c) acc[c] += (src_row[xl + c] + src_row[xr + c]) * w;
                }
                for (int c = 0; c < kChannels; ++c) dst_row[x * kChannels + c] = acc[c];
            }
        };

        blur_clamped(0, interior_begin);
        for (int x = interior_begin; x < interior_end; ++x) {
            const float* centre = src_row + x * kChannels;
            float acc[kChannels];
            for (int c = 0; c < kChannels; ++c) acc[c] = centre[c] * kernel.weights[0];
            for (int t = 1; t <= radius; ++t) {
                const float* left = centre - t * kChannels;
                const float* right = centre + t * kChannels;
                const float w = kernel.weights[t];
                for (int c = 0; c < kChannels; ++c) acc[c] += (left[c] + right[c]) * w;
            }
            for (int c = 0; c < kChannels; ++c) dst_row[x * kChannels + c] = acc[c];
        }
        blur_clamped(interior_end, width);
    }
}

// Taps in the outer loop, pixels in the inner loop: each source row is streamed
// once per tap, which keeps the access pattern linear.
void BlurRowsVertical(const ImageF& src, ImageF& dst, const BlurKernel& kernel, int y_begin, int y_end) {
    const int radius = kernel.radius;
    const int height = src.height;
    const int floats_per_row = src.width * kChannels;

    for (int y = y_begin; y < y_end; ++y) {
        const float* centre_row = reinterpret_cast<const float*>(src.Row(y));
        float* dst_row = reinterpret_cast<float*>(dst.Row(y));
        const float centre_weight = kernel.weights[0];
        for (int i = 0; i < floats_per_row; ++i) dst_row[i] = centre_row[i] * centre_weight;

        for (int t = 1; t <= radius; ++t) {
            const int yu = std::max(y - t, 0);
            const int yd = std::min(y + t, height - 1);
            const float* up = reinterpret_cast<const float*>(src.Row(yu));
            const float* down = reinterpret_cast<const float*>(src.Row(yd));
            const float w = kernel.weights[t];
            for (int i = 0; i < floats_per_row; ++i) dst_row[i] += (up[i] + down[i]) * w;
        }
    }
}

void CopyRows(const ImageF& src, ImageF& dst, int y_begin, int y_end) {
    const std::size_t bytes = static_cast<std::size_t>(src.width) * sizeof(PixelF);
    for (int y = y_begin; y < y_end; ++y) {
        std::memcpy(dst.Row(y), src.Row(y), bytes);
    }
}

}  // namespace

BlurKernel BlurKernel::Gaussian(float sigma, int max_radius) {
    BlurKernel kernel;
    if (!(sigma > 0.0f)) {
        kernel.radius = 0;
        kernel.weights[0] = 1.0f;
        return kernel;
    }
    // Truncating at 4 sigma keeps the discarded weight near 0.006%, so the
    // realised blur matches the requested sigma whatever the ceil() lands on.
    const int radius = std::min(max_radius, std::max(1, static_cast<int>(std::ceil(sigma * 4.0f))));
    kernel.radius = radius;
    const float inv_two_sigma_sq = 1.0f / (2.0f * sigma * sigma);
    float sum = 0.0f;
    for (int t = 0; t <= radius; ++t) {
        const float w = std::exp(-static_cast<float>(t) * static_cast<float>(t) * inv_two_sigma_sq);
        kernel.weights[t] = w;
        sum += (t == 0) ? w : 2.0f * w;
    }
    const float inv_sum = 1.0f / sum;
    for (int t = 0; t <= radius; ++t) kernel.weights[t] *= inv_sum;
    return kernel;
}

void BlurSeparable(ImageF& image, ImageF& temp, const BlurKernel& horizontal, const BlurKernel& vertical,
                   TaskRunner& runner) {
    if (image.Empty()) return;
    if (horizontal.radius <= 0 && vertical.radius <= 0) return;

    ImageF temp_view = temp;
    temp_view.width = image.width;
    temp_view.height = image.height;

    ParallelRows(runner, image.height, [&](int begin, int end, int) {
        if (horizontal.radius > 0) {
            BlurRowsHorizontal(image, temp_view, horizontal, begin, end);
        } else {
            CopyRows(image, temp_view, begin, end);
        }
    });

    ParallelRows(runner, image.height, [&](int begin, int end, int) {
        if (vertical.radius > 0) {
            BlurRowsVertical(temp_view, image, vertical, begin, end);
        } else {
            CopyRows(temp_view, image, begin, end);
        }
    });
}

}  // namespace abglow
