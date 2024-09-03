#pragma once

#include "metal_helper.h"

namespace radray::rhi::metal {

class Device : public DeviceInterface {
public:
    Device(const RadrayDeviceDescriptorMetal& desc);
    ~Device() noexcept override;

    RadrayCommandQueue CreateCommandQueue(RadrayQueueType type) override;
    void DestroyCommandQueue(RadrayCommandQueue queue) override;
    void SubmitQueue(const RadraySubmitQueueDescriptor& desc) override;
    void WaitQueue(RadrayCommandQueue queue) override;

    RadrayFence CreateFence() override;
    void DestroyFence(RadrayFence fence) override;
    RadrayFenceState GetFenceState(RadrayFence fence) override;
    void WaitFences(std::span<const RadrayFence> fences) override;

    RadrayCommandAllocator CreateCommandAllocator(RadrayCommandQueue queue) override;
    void DestroyCommandAllocator(RadrayCommandAllocator alloc) override;
    RadrayCommandList CreateCommandList(RadrayCommandAllocator alloc) override;
    void DestroyCommandList(RadrayCommandList list) override;
    void ResetCommandAllocator(RadrayCommandAllocator alloc) override;
    void BeginCommandList(RadrayCommandList list) override;
    void EndCommandList(RadrayCommandList list) override;

    RadraySwapChain CreateSwapChain(const RadraySwapChainDescriptor& desc) override;
    void DestroySwapChian(RadraySwapChain swapchain) override;

    RadrayBuffer CreateBuffer(const RadrayBufferDescriptor& desc) override;
    void DestroyBuffer(RadrayBuffer buffer) override;
    RadrayBufferView CreateBufferView(const RadrayBufferViewDescriptor& desc) override;
    void DestroyBufferView(RadrayBuffer buffer, RadrayBufferView view) override;

    RadrayTexture CreateTexture(const RadrayTextureDescriptor& desc) override;
    void DestroyTexture(RadrayTexture texture) override;
    RadrayTextureView CreateTextureView(const RadrayTextureViewDescriptor& desc) override;
    void DestroyTextureView(RadrayTextureView view) override;

    RadrayShader CompileShader(const RadrayCompileRasterizationShaderDescriptor& desc) override;
    void DestroyShader(RadrayShader shader) override;

    RadrayRootSignature CreateRootSignature(const RadrayRootSignatureDescriptor& desc) override;
    void DestroyRootSignature(RadrayRootSignature rootSig) override;

    RadrayGraphicsPipeline CreateGraphicsPipeline(const RadrayGraphicsPipelineDescriptor& desc) override;
    void DestroyGraphicsPipeline(RadrayGraphicsPipeline pipe) override;

    uint32_t AcquireNextRenderTarget(RadraySwapChain swapchain) override;
    void Present(RadraySwapChain swapchain) override;

public:
    MTL::Device* device;
};

}  // namespace radray::rhi::metal
