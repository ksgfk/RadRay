#pragma once

#include <radray/rhi/device_interface.h>
#include <radray/rhi/dxc_shader_compiler.h>

#include "d3d12_helper.h"
#include "d3d12_descriptor_heap.h"

namespace radray::rhi::d3d12 {

class Device : public DeviceInterface {
public:
    Device(const RadrayDeviceDescriptorD3D12& desc);
    ~Device() noexcept override = default;

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
    RadrayRenderPassEncoder BeginRenderPass(const RadrayRenderPassDescriptor& desc) override;
    void EndRenderPass(RadrayRenderPassEncoder encoder) override;

    RadraySwapChain CreateSwapChain(const RadraySwapChainDescriptor& desc) override;
    void DestroySwapChian(RadraySwapChain swapchain) override;
    uint32_t AcquireNextRenderTarget(RadraySwapChain swapchain) override;
    void Present(RadraySwapChain swapchain) override;

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

    DxcShaderCompiler* GetDxc();

public:
    ComPtr<IDXGIFactory6> dxgiFactory;
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<ID3D12Device5> device;
    ComPtr<D3D12MA::Allocator> resourceAlloc;
    radray::unique_ptr<DescriptorHeap> cbvSrvUavHeap;
    radray::unique_ptr<DescriptorHeap> rtvHeap;
    radray::unique_ptr<DescriptorHeap> dsvHeap;
    radray::unique_ptr<DescriptorHeap> gpuCbvSrvUavHeap;
    radray::unique_ptr<DescriptorHeap> gpuSamplerHeap;

private:
    radray::unique_ptr<DxcShaderCompiler> _dxc;

public:
    bool canSetDebugName;
};

}  // namespace radray::rhi::d3d12
