#pragma once

#include <mutex>

#include <radray/rhi/device.h>

#include "helper.h"

namespace radray::rhi::d3d12 {

class Device : public IDevice {
public:
    explicit Device(const DeviceCreateInfoD3D12& info);
    ~Device() noexcept;

    uint64_t AddRef() override;

    uint64_t RemoveRef() override;

    RC<ISwapChain> CreateSwapChain(const SwapChainCreateInfo& info) override;

    RC<ICommandQueue> CreateCommandQueue(const CommandQueueCreateInfo& info) override;

    RC<IFence> CreateFence(const FenceCreateInfo& info) override;

    RC<IBuffer> CreateBuffer(const BufferCreateInfo& info) override;

    RC<ITexture> CreateTexture(const TextureCreateInfo& info) override;

private:
    mutable std::mutex _mutex{};
    uint64_t _refCount{0};

public:
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<ID3D12Device5> device;
    ComPtr<IDXGIFactory6> dxgiFactory;
};

}  // namespace radray::rhi::d3d12
