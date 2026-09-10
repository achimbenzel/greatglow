#pragma once

#include <cstddef>

namespace abglow {

// Scratch memory provider. The plug-in routes this to the host allocator so
// After Effects can account for (and reclaim) render memory.
class Allocator {
public:
    virtual ~Allocator() = default;
    virtual void* Allocate(std::size_t bytes) = 0;
    virtual void Free(void* ptr) = 0;
};

}  // namespace abglow
