#pragma once

#include <array>

#include <radray/render/device.h>
#include "d3d12_helper.h"
#include "d3d12_cmd_queue.h"

namespace radray::render::d3d12 {

class DeviceD3D12 : public radray::render::Device {
public:
    DeviceD3D12(
        ComPtr<ID3D12Device> device,
        ComPtr<IDXGIFactory4> dxgiFactory,
        ComPtr<IDXGIAdapter1> dxgiAdapter) noexcept
        : _device(std::move(device)),
          _dxgiFactory(std::move(dxgiFactory)),
          _dxgiAdapter(std::move(dxgiAdapter)) {}
    ~DeviceD3D12() noexcept override;

    bool IsValid() const noexcept override { return _device != nullptr; }
    void Destroy() noexcept override;

    Backend GetBackend() noexcept override { return Backend::D3D12; }

    std::optional<CommandQueue*> GetCommandQueue(QueueType type, uint32_t slot) noexcept override;

    std::optional<radray::shared_ptr<Shader>> CreateShader(
        std::span<const byte> blob,
        const ShaderReflection& refl,
        ShaderStage stage,
        std::string_view entryPoint,
        std::string_view name) noexcept override;

    std::optional<radray::shared_ptr<RootSignature>> CreateRootSignature(std::span<Shader*> shaders) noexcept override;

    std::optional<radray::shared_ptr<GraphicsPipelineState>> CreateGraphicsPipeline(
        const GraphicsPipelineStateDescriptor& desc) noexcept override;

    std::optional<radray::shared_ptr<SwapChain>> CreateSwapChain(
        CommandQueue* presentQueue,
        const void* nativeWindow,
        uint32_t width,
        uint32_t height,
        uint32_t backBufferCount,
        TextureFormat format,
        bool enableSync) noexcept override;

public:
    ComPtr<ID3D12Device> _device;
    ComPtr<IDXGIFactory4> _dxgiFactory;
    ComPtr<IDXGIAdapter1> _dxgiAdapter;
    std::array<radray::vector<radray::unique_ptr<CmdQueueD3D12>>, 3> _queues;
    bool _isAllowTearing = false;
};

std::optional<radray::shared_ptr<DeviceD3D12>> CreateDevice(const D3D12DeviceDescriptor& desc);

}  // namespace radray::render::d3d12
