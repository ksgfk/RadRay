#pragma once

#include <radray/rhi/device_interface.h>

#include "d3d12_helper.h"

namespace radray::rhi::d3d12 {

class Device : public DeviceInterface {
public:
    Device(const RadrayDeviceDescriptorD3D12& desc);
    ~Device() noexcept override = default;

    RadrayCommandQueue CreateCommandQueue(RadrayQueueType type) override;
    void DestroyCommandQueue(RadrayCommandQueue queue) override;

    RadrayFence CreateFence() override;
    void DestroyFence(RadrayFence fence) override;

    RadrayCommandAllocator CreateCommandAllocator(RadrayQueueType type) override;
    void DestroyCommandAllocator(RadrayCommandAllocator alloc) override;
    RadrayCommandList CreateCommandList(RadrayCommandAllocator alloc) override;
    void DestroyCommandList(RadrayCommandList list) override;
    void ResetCommandAllocator(RadrayCommandAllocator alloc) override;

    RadraySwapChain CreateSwapChain(const RadraySwapChainDescriptor& desc) override;
    void DestroySwapChian(RadraySwapChain swapchain) override;

public:
    ComPtr<IDXGIFactory6> dxgiFactory;
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<ID3D12Device5> device;
};

}  // namespace radray::rhi::d3d12
