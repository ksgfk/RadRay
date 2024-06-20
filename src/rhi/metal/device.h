#pragma once

#include <radray/rhi/device_interface.h>
#include "helper.h"

namespace radray::rhi::metal {

class DeviceInterfaceMetal : public DeviceInterface {
public:
    DeviceInterfaceMetal();
    ~DeviceInterfaceMetal() noexcept override;

public:
    NSRef<MTL::Device> device;
};

std::unique_ptr<DeviceInterfaceMetal> CreateImpl(const DeviceCreateInfoMetal& info);

}  // namespace radray::rhi::metal
