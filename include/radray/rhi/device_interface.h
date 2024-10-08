#pragma once

#include <span>

#include <radray/types.h>
#include <radray/rhi/ctypes.h>

namespace radray::rhi {

template <class T, class... Args>
constexpr T* RhiNew(Args&&... args) {
    return new T{std::forward<Args>(args)...};
}

template <class T>
constexpr void RhiDelete(T* ptr) noexcept {
    delete ptr;
}

class DeviceInterface {
public:
    DeviceInterface() = default;
    RADRAY_NO_COPY_CTOR(DeviceInterface);
    RADRAY_NO_MOVE_CTOR(DeviceInterface);
    virtual ~DeviceInterface() noexcept = default;

    virtual RadrayCommandQueue CreateCommandQueue(RadrayQueueType type) = 0;
    virtual void DestroyCommandQueue(RadrayCommandQueue queue) = 0;
    virtual void SubmitQueue(const RadraySubmitQueueDescriptor& desc) = 0;
    virtual void WaitQueue(RadrayCommandQueue queue) = 0;

    virtual RadrayFence CreateFence() = 0;
    virtual void DestroyFence(RadrayFence fence) = 0;
    virtual RadrayFenceState GetFenceState(RadrayFence fence) = 0;
    virtual void WaitFences(std::span<const RadrayFence> fences) = 0;

    virtual RadrayCommandAllocator CreateCommandAllocator(RadrayCommandQueue queue) = 0;
    virtual void DestroyCommandAllocator(RadrayCommandAllocator alloc) = 0;
    virtual RadrayCommandList CreateCommandList(RadrayCommandAllocator alloc) = 0;
    virtual void DestroyCommandList(RadrayCommandList list) = 0;
    virtual void ResetCommandAllocator(RadrayCommandAllocator alloc) = 0;
    virtual void BeginCommandList(RadrayCommandList list) = 0;
    virtual void EndCommandList(RadrayCommandList list) = 0;
    virtual RadrayRenderPassEncoder BeginRenderPass(const RadrayRenderPassDescriptor& desc) = 0;
    virtual void EndRenderPass(RadrayRenderPassEncoder encoder) = 0;
    virtual void ResourceBarriers(RadrayCommandList list, const RadrayResourceBarriersDescriptor& desc) = 0;

    virtual RadraySwapChain CreateSwapChain(const RadraySwapChainDescriptor& desc) = 0;
    virtual void DestroySwapChian(RadraySwapChain swapchain) = 0;
    virtual RadrayTexture AcquireNextRenderTarget(RadraySwapChain swapchain) = 0;
    virtual void Present(RadraySwapChain swapchain, RadrayTexture currentRt) = 0;

    virtual RadrayBuffer CreateBuffer(const RadrayBufferDescriptor& desc) = 0;
    virtual void DestroyBuffer(RadrayBuffer buffer) = 0;
    virtual RadrayBufferView CreateBufferView(const RadrayBufferViewDescriptor& desc) = 0;
    virtual void DestroyBufferView(RadrayBuffer buffer, RadrayBufferView view) = 0;

    virtual RadrayTexture CreateTexture(const RadrayTextureDescriptor& desc) = 0;
    virtual void DestroyTexture(RadrayTexture texture) = 0;
    virtual RadrayTextureView CreateTextureView(const RadrayTextureViewDescriptor& desc) = 0;
    virtual void DestroyTextureView(RadrayTextureView view) = 0;

    virtual RadrayShader CompileShader(const RadrayCompileRasterizationShaderDescriptor& desc) = 0;
    virtual void DestroyShader(RadrayShader shader) = 0;

    virtual RadrayRootSignature CreateRootSignature(const RadrayRootSignatureDescriptor& desc) = 0;
    virtual void DestroyRootSignature(RadrayRootSignature rootSig) = 0;

    virtual RadrayGraphicsPipeline CreateGraphicsPipeline(const RadrayGraphicsPipelineDescriptor& desc) = 0;
    virtual void DestroyGraphicsPipeline(RadrayGraphicsPipeline pipe) = 0;
};

}  // namespace radray::rhi
