#pragma once

#include <radray/rhi/device_interface.h>

#include "d3d12_helper.h"

namespace radray::rhi::d3d12 {

class D3D12Device : public DeviceInterface {
public:
    D3D12Device();
    ~D3D12Device() noexcept override;

    CommandQueueHandle CreateCommandQueue(CommandListType type) override;
    void DestroyCommandQueue(CommandQueueHandle handle) override;

    FenceHandle CreateFence() override;
    void DestroyFence(FenceHandle handle) override;

    SwapChainHandle CreateSwapChain(const SwapChainCreateInfo& info, uint64_t cmdQueueHandle) override;
    void DestroySwapChain(SwapChainHandle handle) override;

    ResourceHandle CreateBuffer(BufferType type, uint64_t size) override;
    void DestroyBuffer(ResourceHandle handle) override;

    ResourceHandle CreateTexture(
        PixelFormat format,
        TextureDimension dim,
        uint32_t width, uint32_t height,
        uint32_t depth,
        uint32_t mipmap) override;
    void DestroyTexture(ResourceHandle handle) override;

    PipelineStateHandle CreateGraphicsPipelineState(
        const GraphicsShaderInfo& shader,
        const GraphicsPipelineStateInfo& info,
        std::span<const InputElementInfo> input) override;
    void DestroyPipelineState(PipelineStateHandle handle) override;

    void StartFrame(CommandQueueHandle queue, SwapChainHandle swapchain) override;
    void FinishFrame(CommandQueueHandle queue, SwapChainHandle swapchain) override;
    void DispatchCommand(CommandQueueHandle queue, CommandList&& cmdList) override;
    void Signal(FenceHandle fence, CommandQueueHandle queue, uint64_t value) override;
    void Wait(FenceHandle fence, CommandQueueHandle queue, uint64_t value) override;
    void Synchronize(FenceHandle fence, uint64_t value) override;

public:
    ComPtr<IDXGIFactory2> dxgiFactory;
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<ID3D12Device5> device;
};

std::unique_ptr<D3D12Device> CreateImpl(const DeviceCreateInfoD3D12& info);

}  // namespace radray::rhi::d3d12
