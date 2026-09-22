#pragma once

#include <cstdint>

#include "Pixel.h"

namespace abglow {

enum class PixelDepth { kBits8, kBits16, kFloat32 };

// How a host image stores colour against alpha. After Effects hands effects
// straight (unpremultiplied) pixels at every depth: the SDK's own sampling
// macros ask for PF_MF_Alpha_STRAIGHT, and preserve_rgb_of_zero_alpha only
// means anything if colour survives under zero alpha. The glow is computed
// premultiplied, so the pipeline converts on the way in and on the way out.
enum class AlphaMode { kStraight, kPremultiplied };

// View of a host image buffer. `rowbytes` is a byte stride so AE worlds can be
// addressed without copying. Channel order is ARGB, matching PF_EffectWorld.
struct HostImage {
    void* data = nullptr;
    int rowbytes = 0;
    int width = 0;
    int height = 0;
    PixelDepth depth = PixelDepth::kBits8;
    AlphaMode alpha = AlphaMode::kStraight;

    bool Empty() const { return data == nullptr || width <= 0 || height <= 0; }

    const std::uint8_t* ConstRow(int y) const {
        return static_cast<const std::uint8_t*>(data) + static_cast<std::ptrdiff_t>(y) * rowbytes;
    }

    std::uint8_t* Row(int y) const {
        return static_cast<std::uint8_t*>(data) + static_cast<std::ptrdiff_t>(y) * rowbytes;
    }
};

template <PixelDepth kDepth>
inline PixelF ReadRowPixel(const void* row, int x);

template <>
inline PixelF ReadRowPixel<PixelDepth::kBits8>(const void* row, int x) {
    const Pixel8& p = static_cast<const Pixel8*>(row)[x];
    constexpr float s = 1.0f / kMaxChannel8;
    return PixelF{p.a * s, p.r * s, p.g * s, p.b * s};
}

template <>
inline PixelF ReadRowPixel<PixelDepth::kBits16>(const void* row, int x) {
    const Pixel16& p = static_cast<const Pixel16*>(row)[x];
    constexpr float s = 1.0f / kMaxChannel16;
    return PixelF{p.a * s, p.r * s, p.g * s, p.b * s};
}

template <>
inline PixelF ReadRowPixel<PixelDepth::kFloat32>(const void* row, int x) {
    return static_cast<const PixelF*>(row)[x];
}

inline PixelF ReadHostPixel(const HostImage& image, const void* row, int x) {
    switch (image.depth) {
        case PixelDepth::kBits8: return ReadRowPixel<PixelDepth::kBits8>(row, x);
        case PixelDepth::kBits16: return ReadRowPixel<PixelDepth::kBits16>(row, x);
        case PixelDepth::kFloat32:
        default: return ReadRowPixel<PixelDepth::kFloat32>(row, x);
    }
}

}  // namespace abglow
