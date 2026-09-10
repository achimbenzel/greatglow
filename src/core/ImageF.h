#pragma once

#include <cassert>
#include <cstddef>

#include "Allocator.h"
#include "Pixel.h"

namespace abglow {

// Non-owning view of interleaved float ARGB pixels.
struct ImageF {
    PixelF* data = nullptr;
    int width = 0;
    int height = 0;
    int stride = 0;  // in pixels

    bool Empty() const { return data == nullptr || width <= 0 || height <= 0; }

    PixelF* Row(int y) {
        assert(y >= 0 && y < height);
        return data + static_cast<std::ptrdiff_t>(y) * stride;
    }

    const PixelF* Row(int y) const {
        assert(y >= 0 && y < height);
        return data + static_cast<std::ptrdiff_t>(y) * stride;
    }

    PixelF& At(int x, int y) { return Row(y)[x]; }
    const PixelF& At(int x, int y) const { return Row(y)[x]; }

    // Edge-clamped fetch, used by filters that read outside the image.
    const PixelF& AtClamped(int x, int y) const {
        const int cx = x < 0 ? 0 : (x >= width ? width - 1 : x);
        const int cy = y < 0 ? 0 : (y >= height ? height - 1 : y);
        return Row(cy)[cx];
    }
};

// Owns a float ARGB buffer obtained from an Allocator.
class OwnedImageF {
public:
    OwnedImageF() = default;
    ~OwnedImageF() { Release(); }

    OwnedImageF(const OwnedImageF&) = delete;
    OwnedImageF& operator=(const OwnedImageF&) = delete;

    bool Allocate(Allocator& allocator, int width, int height) {
        Release();
        if (width <= 0 || height <= 0) return false;
        const std::size_t pixels = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
        void* memory = allocator.Allocate(pixels * sizeof(PixelF));
        if (memory == nullptr) return false;
        allocator_ = &allocator;
        view_.data = static_cast<PixelF*>(memory);
        view_.width = width;
        view_.height = height;
        view_.stride = width;
        return true;
    }

    void Release() {
        if (allocator_ != nullptr && view_.data != nullptr) {
            allocator_->Free(view_.data);
        }
        allocator_ = nullptr;
        view_ = ImageF();
    }

    ImageF& View() { return view_; }
    const ImageF& View() const { return view_; }
    bool Valid() const { return !view_.Empty(); }

private:
    Allocator* allocator_ = nullptr;
    ImageF view_;
};

}  // namespace abglow
