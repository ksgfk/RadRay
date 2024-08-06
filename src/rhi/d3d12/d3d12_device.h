#pragma once

#include <radray/rhi/device_interface.h>

#include "d3d12_helper.h"

namespace radray::rhi::d3d12 {

class D3D12Device : public DeviceInterface {
public:
    D3D12Device(const RadrayDeviceDescriptorD3D12& desc);
    ~D3D12Device() noexcept override = default;

    RadrayCommandQueue CreateCommandQueue(RadrayQueueType type) override;
    void DestroyCommandQueue(RadrayCommandQueue queue) override;

public:
    ComPtr<IDXGIFactory2> dxgiFactory;
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<ID3D12Device5> device;
};

}  // namespace radray::rhi::d3d12
