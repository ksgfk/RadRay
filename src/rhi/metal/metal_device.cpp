#include "metal_device.h"

#include <radray/logger.h>

namespace radray::rhi::metal {

MetalDevice::MetalDevice() = default;

MetalDevice::~MetalDevice() noexcept = default;

std::unique_ptr<MetalDevice> CreateImpl(const DeviceCreateInfoMetal& info) {
    NSRef<NS::Array> all{MTL::CopyAllDevices()};
    auto deviceCount = all->count();
    if (info.DeviceIndex >= deviceCount) {
        RADRAY_ERR_LOG("device index out of range. count = {}", deviceCount);
        return std::unique_ptr<MetalDevice>{};
    }
    MTL::Device* device = all->object<MTL::Device>(info.DeviceIndex);
    RADRAY_INFO_LOG("select metal device: {}", device->name()->utf8String());
    auto result = std::make_unique<MetalDevice>();
    result->device = NSRef{device};
    return result;
}

}  // namespace radray::rhi::metal
