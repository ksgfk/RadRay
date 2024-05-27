#pragma once

#include <radray/rhi/device.h>

#include "helper.h"

namespace radray::rhi::d3d12 {

class Device : public IDevice {
public:
    explicit Device(const DeviceCreateInfoD3D12& info);
    ~Device() noexcept;

    std::shared_ptr<ISwapChain> CreateSwapChain(const SwapChainCreateInfo& info) override;

    std::shared_ptr<ICommandQueue> CreateCommandQueue(const CommandQueueCreateInfo& info) override;

    std::shared_ptr<IFence> CreateFence(const FenceCreateInfo& info) override;

    std::shared_ptr<IBuffer> CreateBuffer(const BufferCreateInfo& info) override;

    std::shared_ptr<ITexture> CreateTexture(const TextureCreateInfo& info) override;

    void WaitFence(ID3D12Fence* fence, uint64_t fenceIndex);

public:
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<ID3D12Device5> device;
    ComPtr<IDXGIFactory6> dxgiFactory;
};

}  // namespace radray::rhi::d3d12
