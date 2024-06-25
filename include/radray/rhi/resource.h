#pragma once

#include <limits>

#include <radray/types.h>

namespace radray::rhi {

constexpr auto InvalidHandle = std::numeric_limits<uint64_t>::max();

struct ResourceHandle {
    uint64_t Handle;
    void* Ptr;

    constexpr bool IsValid() const noexcept { return Handle != InvalidHandle; }

    void Invalidate() noexcept {
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

}  // namespace radray::rhi
