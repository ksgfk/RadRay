#pragma once

#include <memory>
#include <array>

#include <radray/rhi/common.h>

namespace radray::rhi {

class DeviceInterface {
public:
    virtual ~DeviceInterface() noexcept = default;
};

void GetSupportApi(std::array<bool, (size_t)ApiType::MAX_COUNT>& api);

std::unique_ptr<DeviceInterface> CreateDeviceD3D12(const DeviceCreateInfoD3D12& info);

std::unique_ptr<DeviceInterface> CreateDeviceMetal(const DeviceCreateInfoMetal& info);

}  // namespace radray::rhi
