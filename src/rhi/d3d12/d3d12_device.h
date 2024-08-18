#pragma once

#include <radray/rhi/device_interface.h>

#include "d3d12_helper.h"
#include "d3d12_descriptor_heap.h"

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

    RadrayBuffer CreateBuffer(const RadrayBufferDescriptor& desc) override;
    void DestroyBuffer(RadrayBuffer buffer) override;
    RadrayBufferView CreateBufferView(const RadrayBufferViewDescriptor& desc) override;
    void DestroyBufferView(RadrayBuffer buffer, RadrayBufferView view) override;

    RadrayTexture CreateTexture(const RadrayTextureDescriptor& desc) override;
    void DestroyTexture(RadrayTexture texture) override;
    RadrayTextureView CreateTextureView(const RadrayTextureViewDescriptor& desc) override;
    void DestroyTextureView(RadrayTextureView view) override;

public:
    ComPtr<IDXGIFactory6> dxgiFactory;
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<ID3D12Device5> device;
    ComPtr<D3D12MA::Allocator> resourceAlloc;
    RhiUniquePtr<DescriptorHeap> cbvSrvUavHeap;
    RhiUniquePtr<DescriptorHeap> rtvHeap;
    RhiUniquePtr<DescriptorHeap> dsvHeap;
    RhiUniquePtr<DescriptorHeap> gpuCbvSrvUavHeap;
    RhiUniquePtr<DescriptorHeap> gpuSamplerHeap;
};

}  // namespace radray::rhi::d3d12
