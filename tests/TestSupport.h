#pragma once

#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "core/Allocator.h"
#include "core/SourceImage.h"
#include "core/TaskRunner.h"

namespace abglow_test {

class MallocAllocator final : public abglow::Allocator {
public:
    void* Allocate(std::size_t bytes) override {
        ++allocations_;
        return std::malloc(bytes);
    }

    void Free(void* ptr) override {
        if (ptr != nullptr) ++frees_;
        std::free(ptr);
    }

    int allocations() const { return allocations_; }
    int frees() const { return frees_; }

private:
    int allocations_ = 0;
    int frees_ = 0;
};

class ThreadPoolRunner final : public abglow::TaskRunner {
public:
    explicit ThreadPoolRunner(int threads) : threads_(threads > 0 ? threads : 1) {}

    int ThreadCount() const override { return threads_; }

    void ParallelFor(int count, const Body& body) override {
        if (count <= 0) return;
        if (threads_ <= 1) {
            for (int i = 0; i < count; ++i) body(i, 0);
            return;
        }
        std::vector<std::thread> workers;
        const int worker_count = threads_ < count ? threads_ : count;
        workers.reserve(static_cast<std::size_t>(worker_count));
        for (int t = 0; t < worker_count; ++t) {
            workers.emplace_back([&, t]() {
                for (int i = t; i < count; i += worker_count) body(i, t);
            });
        }
        for (std::thread& worker : workers) worker.join();
    }

private:
    int threads_;
};

// Simple owned host image used by the tests in place of a PF_EffectWorld.
class TestImage {
public:
    TestImage() = default;

    TestImage(int width, int height, abglow::PixelDepth depth) { Resize(width, height, depth); }

    void Resize(int width, int height, abglow::PixelDepth depth) {
        const int bytes_per_pixel = depth == abglow::PixelDepth::kBits8
                                        ? 4
                                        : (depth == abglow::PixelDepth::kBits16 ? 8 : 16);
        rowbytes_ = width * bytes_per_pixel;
        storage_.assign(static_cast<std::size_t>(rowbytes_) * static_cast<std::size_t>(height), 0);
        view_.data = storage_.data();
        view_.rowbytes = rowbytes_;
        view_.width = width;
        view_.height = height;
        view_.depth = depth;
    }

    abglow::HostImage& View() { return view_; }
    const abglow::HostImage& View() const { return view_; }

    void SetPixel(int x, int y, const abglow::PixelF& value);
    abglow::PixelF GetPixel(int x, int y) const;

private:
    std::vector<unsigned char> storage_;
    int rowbytes_ = 0;
    abglow::HostImage view_;
};

bool WritePng(const std::string& path, const TestImage& image);

// Nearest-neighbour crop, for inspecting banding and interpolation artefacts.
bool WriteZoomedCrop(const std::string& path, const TestImage& image, int x0, int y0, int width, int height,
                     int zoom);

// Longest run of identical 8-bit values along a scanline: a banding proxy.
int LongestFlatRun(const TestImage& image, int y);

}  // namespace abglow_test
