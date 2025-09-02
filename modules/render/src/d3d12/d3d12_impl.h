#pragma once

#include "d3d12_helper.h"

namespace radray::render::d3d12 {

class DeviceD3D12;

class DeviceD3D12 final : public Device {
public:
    DeviceD3D12(
        ComPtr<ID3D12Device> device,
        ComPtr<IDXGIFactory4> dxgiFactory,
        ComPtr<IDXGIAdapter1> dxgiAdapter,
        ComPtr<D3D12MA::Allocator> mainAlloc) noexcept;

    ~DeviceD3D12() noexcept override;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

    Backend GetBackend() noexcept override { return Backend::D3D12; }

    Nullable<CommandQueue> GetCommandQueue(QueueType type, uint32_t slot) noexcept override;

    Nullable<shared_ptr<CommandBuffer>> CreateCommandBuffer(CommandQueue* queue) noexcept override;

    Nullable<shared_ptr<Fence>> CreateFence(uint64_t initValue) noexcept override;

    Nullable<shared_ptr<SwapChain>> CreateSwapChain(const SwapChainDescriptor& desc) noexcept override;

    Nullable<shared_ptr<Buffer>> CreateBuffer(const BufferDescriptor& desc) noexcept override;

    Nullable<shared_ptr<BufferView>> CreateBufferView(const BufferViewDescriptor& desc) noexcept override;

    Nullable<shared_ptr<Texture>> CreateTexture(const TextureDescriptor& desc) noexcept override;

    Nullable<shared_ptr<TextureView>> CreateTextureView(const TextureViewDescriptor& desc) noexcept override;

    Nullable<shared_ptr<Shader>> CreateShader(const ShaderDescriptor& desc) noexcept override;

    Nullable<shared_ptr<RootSignature>> CreateRootSignature(const RootSignatureDescriptor& desc) noexcept override;

    Nullable<shared_ptr<GraphicsPipelineState>> CreateGraphicsPipelineState(const GraphicsPipelineStateDescriptor& desc) noexcept override;

public:
    void DestroyImpl() noexcept;

    ComPtr<ID3D12Device> _device;
    ComPtr<IDXGIFactory4> _dxgiFactory;
    ComPtr<IDXGIAdapter1> _dxgiAdapter;
    ComPtr<D3D12MA::Allocator> _mainAlloc;
    CD3DX12FeatureSupport _features;
    bool _isAllowTearing = false;
};

Nullable<shared_ptr<DeviceD3D12>> CreateDevice(const D3D12DeviceDescriptor& desc);

}  // namespace radray::render::d3d12
