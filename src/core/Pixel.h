#pragma once

#include <cstdint>

namespace abglow {

// Channel order matches After Effects' ARGB pixel layout.
struct PixelF {
    float a, r, g, b;
};

struct Pixel8 {
    std::uint8_t a, r, g, b;
};

struct Pixel16 {
    std::uint16_t a, r, g, b;
};

constexpr float kMaxChannel8 = 255.0f;
constexpr float kMaxChannel16 = 32768.0f;  // PF_MAX_CHAN16

// Rec.709 luminance weights.
constexpr float kLumaR = 0.2126f;
constexpr float kLumaG = 0.7152f;
constexpr float kLumaB = 0.0722f;

inline float Luminance(float r, float g, float b) {
    return kLumaR * r + kLumaG * g + kLumaB * b;
}

inline float Luminance(const PixelF& p) {
    return Luminance(p.r, p.g, p.b);
}

}  // namespace abglow
