#include "AeAdapters.h"

#include <cstdint>
#include <cstring>
#include <new>

namespace abglow {
namespace {

constexpr std::size_t kAlignment = 32;
constexpr std::size_t kHeaderSize = kAlignment;

struct AllocationHeader {
    PF_Handle handle;
};

}  // namespace

void* AeAllocator::Allocate(std::size_t bytes) {
    if (handle_suite_ == nullptr || bytes == 0) return nullptr;

    const std::size_t total = bytes + kHeaderSize + kAlignment;
    PF_Handle handle = handle_suite_->host_new_handle(static_cast<A_HandleSize>(total));
    if (handle == nullptr) return nullptr;

    void* base = handle_suite_->host_lock_handle(handle);
    if (base == nullptr) {
        handle_suite_->host_dispose_handle(handle);
        return nullptr;
    }

    auto address = reinterpret_cast<std::uintptr_t>(base) + kHeaderSize;
    address = (address + kAlignment - 1) & ~(static_cast<std::uintptr_t>(kAlignment) - 1);
    void* payload = reinterpret_cast<void*>(address);
    AllocationHeader header{handle};
    std::memcpy(static_cast<char*>(payload) - sizeof(AllocationHeader), &header, sizeof(header));
    return payload;
}

void AeAllocator::Free(void* ptr) {
    if (ptr == nullptr || handle_suite_ == nullptr) return;
    AllocationHeader header{};
    std::memcpy(&header, static_cast<char*>(ptr) - sizeof(AllocationHeader), sizeof(header));
    if (header.handle == nullptr) return;
    handle_suite_->host_unlock_handle(header.handle);
    handle_suite_->host_dispose_handle(header.handle);
}

namespace {

struct ProbeContext {
    A_long count = 1;
};

PF_Err ProbeThreadCount(void* refcon, A_long thread_index, A_long index, A_long count) {
    (void)thread_index;
    (void)index;
    ProbeContext* probe = static_cast<ProbeContext*>(refcon);
    if (probe != nullptr && count > probe->count) probe->count = count;
    return PF_Err_NONE;
}

struct DispatchContext {
    const TaskRunner::Body* body = nullptr;
};

}  // namespace

AeTaskRunner::AeTaskRunner(PF_InData* in_data, PF_Iterate8Suite1* iterate_suite)
    : in_data_(in_data), iterate_suite_(iterate_suite) {
    if (iterate_suite_ == nullptr || iterate_suite_->iterate_generic == nullptr) return;
    ProbeContext probe;
    if (iterate_suite_->iterate_generic(PF_Iterations_ONCE_PER_PROCESSOR, &probe, ProbeThreadCount) ==
        PF_Err_NONE) {
        thread_count_ = probe.count > 0 ? static_cast<int>(probe.count) : 1;
    }
}

PF_Err AeTaskRunner::Dispatch(void* refcon, A_long thread_index, A_long index, A_long count) {
    (void)count;
    DispatchContext* context = static_cast<DispatchContext*>(refcon);
    if (context == nullptr || context->body == nullptr) return PF_Err_NONE;
    // Never let an exception unwind through the host's thread pool.
    try {
        (*context->body)(static_cast<int>(index), static_cast<int>(thread_index));
    } catch (...) {
        return PF_Err_INTERNAL_STRUCT_DAMAGED;
    }
    return PF_Err_NONE;
}

void AeTaskRunner::ParallelFor(int count, const Body& body) {
    if (count <= 0) return;
    if (iterate_suite_ == nullptr || iterate_suite_->iterate_generic == nullptr) {
        for (int i = 0; i < count; ++i) body(i, 0);
        return;
    }
    DispatchContext context;
    context.body = &body;
    iterate_suite_->iterate_generic(count, &context, Dispatch);
}

PixelDepth DepthFromBitsPerChannel(short bits_per_channel) {
    switch (bits_per_channel) {
        case 16: return PixelDepth::kBits16;
        case 32: return PixelDepth::kFloat32;
        case 8:
        default: return PixelDepth::kBits8;
    }
}

HostImage MakeHostImage(PF_EffectWorld* world, PixelDepth depth) {
    HostImage image;
    if (world == nullptr || world->data == nullptr) return image;
    image.data = world->data;
    image.rowbytes = world->rowbytes;
    image.width = world->width;
    image.height = world->height;
    image.depth = depth;
    // After Effects' worlds are straight at every depth; the pipeline works
    // premultiplied and converts at its edges.
    image.alpha = AlphaMode::kStraight;
    return image;
}

}  // namespace abglow
