#pragma once

#include "SdkIncludes.h"
#include "core/Allocator.h"
#include "core/SourceImage.h"
#include "core/TaskRunner.h"

namespace abglow {

// Scratch memory from the host, so After Effects can account for it and purge
// caches when memory runs short.
class AeAllocator final : public Allocator {
public:
    AeAllocator(PF_InData* in_data, PF_HandleSuite1* handle_suite)
        : in_data_(in_data), handle_suite_(handle_suite) {}

    void* Allocate(std::size_t bytes) override;
    void Free(void* ptr) override;

private:
    PF_InData* in_data_ = nullptr;
    PF_HandleSuite1* handle_suite_ = nullptr;
};

// Runs work on the host's thread pool. Using it instead of our own threads
// keeps multi-frame rendering from oversubscribing the machine.
class AeTaskRunner final : public TaskRunner {
public:
    AeTaskRunner(PF_InData* in_data, PF_Iterate8Suite1* iterate_suite);

    int ThreadCount() const override { return thread_count_; }
    void ParallelFor(int count, const Body& body) override;

private:
    static PF_Err Dispatch(void* refcon, A_long thread_index, A_long index, A_long count);

    PF_InData* in_data_ = nullptr;
    PF_Iterate8Suite1* iterate_suite_ = nullptr;
    int thread_count_ = 1;
};

// SPBasicSuite hands back a const pointer; suites are call tables, so the
// const is not meaningful to the caller.
template <typename Suite>
Suite* AcquireSuite(SPBasicSuite* basic, const char* name, int version) {
    if (basic == nullptr) return nullptr;
    const void* suite = nullptr;
    if (basic->AcquireSuite(name, version, &suite) != kSPNoError) return nullptr;
    return const_cast<Suite*>(static_cast<const Suite*>(suite));
}

PixelDepth DepthFromBitsPerChannel(short bits_per_channel);

HostImage MakeHostImage(PF_EffectWorld* world, PixelDepth depth);

}  // namespace abglow
