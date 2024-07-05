#pragma once

#include <limits>

#include <radray/types.h>

namespace radray::rhi {

static_assert(sizeof(uint64_t) == sizeof(void*), "ptr is not 64bit");

constexpr auto InvalidHandle = std::numeric_limits<uint64_t>::max();

struct ResourceHandle {
    uint64_t Handle;
    void* Ptr;

    constexpr bool IsValid() const noexcept { return Handle != InvalidHandle; }

    constexpr void Invalidate() noexcept {
        Handle = InvalidHandle;
        Ptr = nullptr;
    }
};

struct CommandQueueHandle : public ResourceHandle {};

struct SwapChainHandle : public ResourceHandle {};

struct BufferHandle : public ResourceHandle {
    size_t ByteSize;
    size_t Stride;
};

struct TextureHandle : public ResourceHandle {};

struct FenceHandle : public ResourceHandle {};

}  // namespace radray::rhi
