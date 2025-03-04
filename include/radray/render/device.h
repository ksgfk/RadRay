#pragma once

#include <optional>
#include <span>

#include <radray/types.h>
#include <radray/utility.h>
#include <radray/render/common.h>
#include <radray/render/root_signature.h>
#include <radray/render/pipeline_state.h>

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

class Device : public radray::enable_shared_from_this<Device>, public RenderBase {
public:
    virtual ~Device() noexcept = default;

    virtual Backend GetBackend() noexcept = 0;

    virtual Nullable<CommandQueue> GetCommandQueue(QueueType type, uint32_t slot = 0) noexcept = 0;

    virtual Nullable<radray::shared_ptr<Fence>> CreateFence() noexcept = 0;

    virtual Nullable<radray::shared_ptr<Shader>> CreateShader(
        std::span<const byte> blob,
        ShaderBlobCategory category,
        ShaderStage stage,
        std::string_view entryPoint,
        std::string_view name) noexcept = 0;

    // TODO: no auto root sig layout
    virtual Nullable<radray::shared_ptr<RootSignature>> CreateRootSignature(std::span<Shader*> shaders) noexcept = 0;

    virtual Nullable<radray::shared_ptr<GraphicsPipelineState>> CreateGraphicsPipeline(
        const GraphicsPipelineStateDescriptor& desc) noexcept = 0;

    virtual Nullable<radray::shared_ptr<SwapChain>> CreateSwapChain(
        CommandQueue* presentQueue,
        const void* nativeWindow,
        uint32_t width,
        uint32_t height,
        uint32_t backBufferCount,
        TextureFormat format,
        bool enableSync) noexcept = 0;

    virtual Nullable<radray::shared_ptr<Buffer>> CreateBuffer(
        uint64_t size,
        ResourceType type,
        ResourceUsage usage,
        ResourceStates initState,
        ResourceMemoryTips tips,
        std::string_view name = {}) noexcept = 0;

    virtual Nullable<radray::shared_ptr<Texture>> CreateTexture(
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
        ResourceMemoryTips tips,
        std::string_view name = {}) noexcept = 0;

    virtual Nullable<radray::shared_ptr<BufferView>> CreateBufferView(
        Buffer* buffer,
        ResourceType type,
        TextureFormat format,
        uint64_t offset,
        uint32_t count,
        uint32_t stride) noexcept = 0;

    virtual Nullable<radray::shared_ptr<TextureView>> CreateTextureView(
        Texture* texture,
        ResourceType type,
        TextureFormat format,
        TextureDimension dim,
        uint32_t baseArrayLayer,
        uint32_t arrayLayerCount,
        uint32_t baseMipLevel,
        uint32_t mipLevelCount) noexcept = 0;

    // TODO: no, we need input desc set layout by use, not by root sig
    virtual Nullable<radray::shared_ptr<DescriptorSet>> CreateDescriptorSet(
        RootSignature* rootSignature,
        uint32_t set) noexcept = 0;
};

Nullable<radray::shared_ptr<Device>> CreateDevice(const DeviceDescriptor& desc);

}  // namespace radray::render
