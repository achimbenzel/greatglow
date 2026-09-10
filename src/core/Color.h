#pragma once

#include <cmath>

#include "Pixel.h"

namespace abglow {

// sRGB transfer functions, extended symmetrically so HDR and negative values
// survive a round trip.
inline float SrgbToLinear(float c) {
    const float s = c < 0.0f ? -1.0f : 1.0f;
    const float a = std::fabs(c);
    if (a <= 0.04045f) return s * a / 12.92f;
    return s * std::pow((a + 0.055f) / 1.055f, 2.4f);
}

inline float LinearToSrgb(float c) {
    const float s = c < 0.0f ? -1.0f : 1.0f;
    const float a = std::fabs(c);
    if (a <= 0.0031308f) return s * a * 12.92f;
    return s * (1.055f * std::pow(a, 1.0f / 2.4f) - 0.055f);
}

// Linear gain applied to the glow: exposure, intensity, saturation and tint all
// compose into one affine operator, so they can run at pyramid resolution.
struct GlowColorTransform {
    float gain = 1.0f;
    float saturation = 1.0f;
    float tint_r = 1.0f;
    float tint_g = 1.0f;
    float tint_b = 1.0f;

    void Apply(PixelF& p) const {
        float r = p.r;
        float g = p.g;
        float b = p.b;
        if (saturation != 1.0f) {
            const float luma = Luminance(r, g, b);
            r = luma + (r - luma) * saturation;
            g = luma + (g - luma) * saturation;
            b = luma + (b - luma) * saturation;
        }
        p.r = r * gain * tint_r;
        p.g = g * gain * tint_g;
        p.b = b * gain * tint_b;
        p.a *= gain;
    }
};

}  // namespace abglow
