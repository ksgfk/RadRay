#pragma once

#include <memory>

#include <radray/types.h>
#include <radray/rhi/descriptors.h>

namespace radray::rhi {

class Device : public std::enable_shared_from_this<Device> {
public:
    RADRAY_NO_COPY_CTOR(Device);
    RADRAY_NO_MOVE_CTOR(Device);
    virtual ~Device() noexcept = default;

    virtual RadrayCommandQueue CreateCommandQueue(RadrayQueueType type) = 0;
    virtual void DestroyCommandQueue(RadrayCommandQueue queue) = 0;
};

std::shared_ptr<Device> CreateDeviceD3D12(const RadrayDeviceDescriptorD3D12* desc);
std::shared_ptr<Device> CreateDeviceMetal(const RadrayDeviceDescriptorMetal* desc);

}  // namespace radray::rhi
