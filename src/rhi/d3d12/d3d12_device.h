#pragma once

#include <radray/rhi/device_interface.h>

#include "d3d12_helper.h"

namespace radray::rhi::d3d12 {

class D3D12Device : public DeviceInterface {
public:
    D3D12Device();
    ~D3D12Device() noexcept override;

    CommandQueueHandle CreateCommandQueue(CommandListType type) override;
    void DestroyCommandQueue(const CommandQueueHandle& handle) override;

    SwapChainHandle CreateSwapChain(const SwapChainCreateInfo& info, uint64_t cmdQueueHandle) override;
    void DestroySwapChain(const SwapChainHandle& handle) override;

public:
    ComPtr<IDXGIFactory2> dxgiFactory;
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<ID3D12Device5> device;
};

std::unique_ptr<D3D12Device> CreateImpl(const DeviceCreateInfoD3D12& info);

}  // namespace radray::rhi::d3d12
