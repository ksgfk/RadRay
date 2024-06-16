#pragma once

#include <radray/rhi/device_interface.h>

#include "helper.h"

namespace radray::rhi::d3d12 {

class DeviceInterfaceD3D12 : public DeviceInterface {
public:
    DeviceInterfaceD3D12();
    ~DeviceInterfaceD3D12() noexcept override;

public:
    ComPtr<IDXGIFactory2> dxgiFactory;
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<ID3D12Device5> device;
};

std::unique_ptr<DeviceInterfaceD3D12> CreateImpl(const DeviceCreateInfoD3D12& info);

}  // namespace radray::rhi::d3d12
