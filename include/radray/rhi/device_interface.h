#pragma once

#include <radray/types.h>
#include <radray/rhi/ctypes.h>

namespace radray::rhi {

class DeviceInterface {
public:
    DeviceInterface() = default;
    RADRAY_NO_COPY_CTOR(DeviceInterface);
    RADRAY_NO_MOVE_CTOR(DeviceInterface);
    virtual ~DeviceInterface() noexcept = default;

    virtual RadrayCommandQueue CreateCommandQueue(RadrayQueueType type) = 0;
    virtual void DestroyCommandQueue(RadrayCommandQueue queue) = 0;

    virtual RadrayFence CreateFence() = 0;
    virtual void DestroyFence(RadrayFence fence) = 0;

    virtual RadrayCommandAllocator CreateCommandAllocator(RadrayQueueType type) = 0;
    virtual void DestroyCommandAllocator(RadrayCommandAllocator alloc) = 0;
    virtual RadrayCommandList CreateCommandList(RadrayCommandAllocator alloc) = 0;
    virtual void DestroyCommandList(RadrayCommandList list) = 0;
    virtual void ResetCommandAllocator(RadrayCommandAllocator alloc) = 0;
};

}  // namespace radray::rhi
