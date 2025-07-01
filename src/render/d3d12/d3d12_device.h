#pragma once

#include <array>

#include <radray/render/device.h>
#include "d3d12_helper.h"
#include "d3d12_cmd_queue.h"
#include "d3d12_descriptor_heap.h"

namespace radray::render::d3d12 {

class DeviceD3D12 : public radray::render::Device {
public:
    DeviceD3D12(
        ComPtr<ID3D12Device> device,
        ComPtr<IDXGIFactory4> dxgiFactory,
        ComPtr<IDXGIAdapter1> dxgiAdapter,
        ComPtr<D3D12MA::Allocator> mainAlloc) noexcept;
    ~DeviceD3D12() noexcept override;

    bool IsValid() const noexcept override { return _device != nullptr; }
    void Destroy() noexcept override;

    Backend GetBackend() noexcept override { return Backend::D3D12; }

    Nullable<CommandQueue> GetCommandQueue(QueueType type, uint32_t slot) noexcept override;

    Nullable<shared_ptr<Fence>> CreateFence() noexcept override;

    Nullable<shared_ptr<Shader>> CreateShader(
        std::span<const byte> blob,
        ShaderBlobCategory category,
        ShaderStage stage,
        std::string_view entryPoint,
        std::string_view name) noexcept override;

    Nullable<shared_ptr<RootSignature>> CreateRootSignature(const RootSignatureDescriptor& info) noexcept override;

    Nullable<shared_ptr<GraphicsPipelineState>> CreateGraphicsPipeline(
        const GraphicsPipelineStateDescriptor& desc) noexcept override;

    Nullable<shared_ptr<SwapChain>> CreateSwapChain(
        CommandQueue* presentQueue,
        const void* nativeWindow,
        uint32_t width,
        uint32_t height,
        uint32_t backBufferCount,
        TextureFormat format,
        bool enableSync) noexcept override;

    Nullable<shared_ptr<Buffer>> CreateBuffer(
        uint64_t size,
        ResourceType type,
        ResourceMemoryUsage usage,
        ResourceStates initState,
        ResourceHints tips,
        std::string_view name = {}) noexcept override;

    Nullable<shared_ptr<Texture>> CreateTexture(
        uint64_t width,
        uint64_t height,
        uint64_t depth,
        uint32_t arraySize,
        TextureFormat format,
        uint32_t mipLevels,
        uint32_t sampleCount,
        uint32_t sampleQuality,
        ClearValue clearValue,
        ResourceType type,
        ResourceStates initState,
        ResourceHints tips,
        std::string_view name = {}) noexcept override;

    Nullable<shared_ptr<Texture>> CreateTexture(const TextureCreateDescriptor& desc) noexcept override;

    Nullable<shared_ptr<ResourceView>> CreateBufferView(
        Buffer* buffer,
        ResourceType type,
        TextureFormat format,
        uint64_t offset,
        uint32_t count,
        uint32_t stride) noexcept override;

    Nullable<shared_ptr<ResourceView>> CreateTextureView(
        Texture* texture,
        ResourceType type,
        TextureFormat format,
        TextureViewDimension dim,
        uint32_t baseArrayLayer,
        uint32_t arrayLayerCount,
        uint32_t baseMipLevel,
        uint32_t mipLevelCount) noexcept override;

    Nullable<shared_ptr<DescriptorSet>> CreateDescriptorSet(const DescriptorSetElementInfo& info) noexcept override;

    Nullable<shared_ptr<Sampler>> CreateSampler(const SamplerDescriptor& desc) noexcept override;

    uint64_t GetUploadBufferNeedSize(Resource* copyDst, uint32_t mipLevel, uint32_t arrayLayer, uint32_t layerCount) const noexcept override;

    void CopyDataToUploadBuffer(
        Resource* dst,
        const void* src,
        size_t srcSize,
        uint32_t mipLevel,
        uint32_t arrayLayer,
        uint32_t layerCount) const noexcept override;

    const CD3DX12FeatureSupport& GetFeatures() const noexcept { return _features; }

    CpuDescriptorAllocator* GetCpuResAllocator() noexcept;
    CpuDescriptorAllocator* GetRtvAllocator() noexcept;
    CpuDescriptorAllocator* GetDsvAllocator() noexcept;
    CpuDescriptorAllocator* GetCpuSamplerAllocator() noexcept;
    GpuDescriptorAllocator* GetGpuResAllocator() noexcept;
    GpuDescriptorAllocator* GetGpuSamplerAllocator() noexcept;

public:
    ComPtr<ID3D12Device> _device;
    ComPtr<IDXGIFactory4> _dxgiFactory;
    ComPtr<IDXGIAdapter1> _dxgiAdapter;
    ComPtr<D3D12MA::Allocator> _mainAlloc;
    std::array<vector<unique_ptr<CmdQueueD3D12>>, (size_t)QueueType::MAX_COUNT> _queues;
    unique_ptr<CpuDescriptorAllocator> _cpuResAlloc;
    unique_ptr<CpuDescriptorAllocator> _cpuRtvAlloc;
    unique_ptr<CpuDescriptorAllocator> _cpuDsvAlloc;
    unique_ptr<CpuDescriptorAllocator> _cpuSamplerAlloc;
    unique_ptr<GpuDescriptorAllocator> _gpuResHeap;
    unique_ptr<GpuDescriptorAllocator> _gpuSamplerHeap;
    CD3DX12FeatureSupport _features;
    bool _isAllowTearing = false;
};

Nullable<shared_ptr<DeviceD3D12>> CreateDevice(const D3D12DeviceDescriptor& desc);

}  // namespace radray::render::d3d12
