#pragma once

#include <optional>
#include <variant>
#include <span>

#include <radray/types.h>
#include <radray/utility.h>
#include <radray/render/common.h>
#include <radray/render/dxc.h>
#include <radray/render/spvc.h>
#include <radray/render/root_signature.h>

namespace radray::render {

class D3D12DeviceDescriptor {
public:
    std::optional<uint32_t> AdapterIndex;
    bool IsEnableDebugLayer;
    bool IsEnableGpuBasedValid;
};

class MetalDeviceDescriptor {
public:
    std::optional<uint32_t> DeviceIndex;
};

class VulkanDeviceDescriptor {
public:
};

using DeviceDescriptor = std::variant<D3D12DeviceDescriptor, MetalDeviceDescriptor, VulkanDeviceDescriptor>;

using ShaderReflection = std::variant<DxilReflection, SpirvReflection, MslReflection>;

class CommandQueue;
class Shader;
class RootSignature;
class GraphicsPipelineState;
class SwapChain;

class Device : public radray::enable_shared_from_this<Device>, public RenderBase {
public:
    virtual ~Device() noexcept = default;

    virtual Backend GetBackend() noexcept = 0;

    virtual std::optional<CommandQueue*> GetCommandQueue(QueueType type, uint32_t slot = 0) noexcept = 0;

    virtual std::optional<radray::shared_ptr<Shader>> CreateShader(
        std::span<const byte> blob,
        const ShaderReflection& refl,
        ShaderStage stage,
        std::string_view entryPoint,
        std::string_view name) noexcept = 0;

    virtual std::optional<radray::shared_ptr<RootSignature>> CreateRootSignature(std::span<Shader*> shaders) noexcept = 0;

    virtual std::optional<radray::shared_ptr<GraphicsPipelineState>> CreateGraphicsPipeline(
        const GraphicsPipelineStateDescriptor& desc) noexcept = 0;

    virtual std::optional<radray::shared_ptr<SwapChain>> CreateSwapChain(
        CommandQueue* presentQueue,
        const void* nativeWindow,
        uint32_t width,
        uint32_t height,
        uint32_t backBufferCount,
        TextureFormat format,
        bool enableSync) noexcept = 0;
};

std::optional<radray::shared_ptr<Device>> CreateDevice(const DeviceDescriptor& desc);

}  // namespace radray::render
