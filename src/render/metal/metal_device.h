#pragma once

#include <radray/render/device.h>
#include "metal_helper.h"
#include "metal_cmd_queue.h"

namespace radray::render::metal {

class DeviceMetal : public radray::render::Device {
public:
    explicit DeviceMetal(NS::SharedPtr<MTL::Device> device) noexcept : _device(std::move(device)) {}
    ~DeviceMetal() noexcept override = default;

    bool IsValid() const noexcept override { return _device.get() != nullptr; }
    void Destroy() noexcept override;

    Backend GetBackend() noexcept override { return Backend::Metal; }

    std::optional<CommandQueue*> GetCommandQueue(QueueType type, uint32_t slot) noexcept override;

    std::optional<shared_ptr<Shader>> CreateShader(
        std::span<const byte> blob,
        const ShaderReflection& refl,
        ShaderStage stage,
        std::string_view entryPoint,
        std::string_view name) noexcept override;

    std::optional<shared_ptr<RootSignature>> CreateRootSignature(std::span<Shader*> shaders) noexcept override;

    std::optional<shared_ptr<GraphicsPipelineState>> CreateGraphicsPipeline(
        const GraphicsPipelineStateDescriptor& desc) noexcept override;

public:
    NS::SharedPtr<MTL::Device> _device;
    std::array<vector<unique_ptr<CmdQueueMetal>>, 3> _queues;
};

std::optional<shared_ptr<DeviceMetal>> CreateDevice(const MetalDeviceDescriptor& desc) noexcept;

}  // namespace radray::render::metal
