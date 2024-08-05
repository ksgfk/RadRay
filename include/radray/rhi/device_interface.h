#pragma once

#include <memory>
#include <memory_resource>

#include <radray/types.h>
#include <radray/rhi/ctypes.h>

namespace radray::rhi {

class DeviceMemoryResource : public std::pmr::memory_resource {
public:
    explicit DeviceMemoryResource(const RadrayDeviceMemoryManagementDescriptor& desc);
    ~DeviceMemoryResource() noexcept override = default;

private:
    void* do_allocate(size_t bytes, size_t align) override;
    void do_deallocate(void* ptr, size_t bytes, size_t align) override;
    bool do_is_equal(const memory_resource& that) const noexcept override;

    RadrayDeviceMemoryManagementDescriptor _desc;
};

class DeviceInterface : public std::enable_shared_from_this<DeviceInterface> {
public:
    DeviceInterface() = default;
    RADRAY_NO_COPY_CTOR(DeviceInterface);
    RADRAY_NO_MOVE_CTOR(DeviceInterface);
    virtual ~DeviceInterface() noexcept = default;

    virtual RadrayCommandQueue CreateCommandQueue(RadrayQueueType type) = 0;
    virtual void DestroyCommandQueue(RadrayCommandQueue queue) = 0;
};

std::shared_ptr<DeviceInterface> CreateDeviceD3D12(const RadrayDeviceDescriptorD3D12* desc);
std::shared_ptr<DeviceInterface> CreateDeviceMetal(const RadrayDeviceDescriptorMetal* desc);

}  // namespace radray::rhi
