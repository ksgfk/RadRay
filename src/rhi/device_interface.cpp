#include <radray/rhi/device_interface.h>

#include <radray/platform.h>
#include <radray/logger.h>

#ifdef RADRAY_ENABLE_D3D12
#include "d3d12/d3d12_device.h"
#endif

namespace radray::rhi {

DeviceMemoryResource::DeviceMemoryResource(const RadrayDeviceMemoryManagementDescriptor& desc)
    : _desc(desc) {
    if (_desc.Alloc == nullptr) {
        _desc.Alloc = [](size_t size, size_t align, void* userPtr) {
            return AlignedAlloc(align, size);
        };
    }
    if (_desc.Release == nullptr) {
        _desc.Release = [](void* ptr, void* userPtr) {
            AlignedFree(ptr);
        };
    }
}

void* DeviceMemoryResource::do_allocate(size_t bytes, size_t align) {
    return _desc.Alloc(bytes, align, _desc.UserPtr);
}

void DeviceMemoryResource::do_deallocate(void* ptr, size_t bytes, size_t align) {
    _desc.Release(ptr, _desc.UserPtr);
}

bool DeviceMemoryResource::do_is_equal(const std::pmr::memory_resource& that) const noexcept {
    return this == &that;
}

std::shared_ptr<DeviceInterface> CreateDeviceD3D12(const RadrayDeviceDescriptorD3D12* desc) {
#ifdef RADRAY_ENABLE_D3D12
    return d3d12::CreateImpl(desc);
#else
    RADRAY_ERR_LOG("cannot create D3D12 device. D3D12 disabled");
    return nullptr;
#endif
}

std::shared_ptr<DeviceInterface> CreateDeviceMetal(const RadrayDeviceDescriptorMetal* desc) {
#ifdef RADRAY_ENABLE_METAL
    return metal::CreateImpl(info);
#else
    RADRAY_ERR_LOG("cannot create Metal device. Metal disabled");
    return nullptr;
#endif
}

}  // namespace radray::rhi
