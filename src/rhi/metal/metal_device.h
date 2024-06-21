#pragma once

#include <radray/rhi/device_interface.h>
#include "metal_helper.h"

namespace radray::rhi::metal {

class MetalDevice : public DeviceInterface {
public:
    MetalDevice();
    ~MetalDevice() noexcept override;

public:
    NSRef<MTL::Device> device;
};

std::unique_ptr<MetalDevice> CreateImpl(const DeviceCreateInfoMetal& info);

}  // namespace radray::rhi::metal
