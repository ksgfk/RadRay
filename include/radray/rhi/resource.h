#pragma once

#include <limits>

#include <radray/types.h>

namespace radray::rhi {

constexpr auto InvalidHandle = std::numeric_limits<uint64_t>::max();

struct ResourceHandle {
    uint64_t Handle;
    void* Ptr;

    constexpr bool IsValid() const noexcept { return Handle != InvalidHandle; }

    void invalidate() noexcept {
        Handle = InvalidHandle;
        Ptr = nullptr;
    }
};

struct SwapChainHandle : public ResourceHandle {
};

}  // namespace radray::rhi
