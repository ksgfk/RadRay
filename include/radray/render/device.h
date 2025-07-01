#pragma once

#include <optional>
#include <span>

#include <radray/types.h>
#include <radray/utility.h>

#include <radray/render/common.h>

#include <radray/render/command_buffer.h>
#include <radray/render/command_encoder.h>
#include <radray/render/command_queue.h>
#include <radray/render/descriptor_set.h>
#include <radray/render/fence.h>
#include <radray/render/pipeline_state.h>
#include <radray/render/resource.h>
#include <radray/render/root_signature.h>
#include <radray/render/sampler.h>
#include <radray/render/shader.h>
#include <radray/render/swap_chain.h>

namespace radray::render {

class D3D12BackendInitDescriptor {
public:
};

class MetalBackendInitDescriptor {
public:
};

class VulkanBackendInitDdescriptor {
public:
    bool IsEnableDebugLayer;
    bool IsEnableGpuBasedValid;
};

using BackendInitDescriptor = std::variant<D3D12BackendInitDescriptor, MetalBackendInitDescriptor, VulkanBackendInitDdescriptor>;

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

struct VulkanCommandQueueDescriptor {
    QueueType Type;
    uint32_t Count;
};

class VulkanDeviceDescriptor {
public:
    std::optional<uint32_t> PhysicalDeviceIndex;
    std::span<VulkanCommandQueueDescriptor> Queues;
};

using DeviceDescriptor = std::variant<D3D12DeviceDescriptor, MetalDeviceDescriptor, VulkanDeviceDescriptor>;

class Device : public enable_shared_from_this<Device>, public RenderBase {
public:
    virtual ~Device() noexcept = default;

    RenderObjectTags GetTag() const noexcept final { return RenderObjectTag::Device; }

    virtual Backend GetBackend() noexcept = 0;

    virtual Nullable<CommandQueue> GetCommandQueue(QueueType type, uint32_t slot = 0) noexcept = 0;

    virtual Nullable<shared_ptr<Fence>> CreateFence() noexcept = 0;

    virtual Nullable<shared_ptr<Shader>> CreateShader(
        std::span<const byte> blob,
        ShaderBlobCategory category,
        ShaderStage stage,
        std::string_view entryPoint,
        std::string_view name) noexcept = 0;

    virtual Nullable<shared_ptr<RootSignature>> CreateRootSignature(const RootSignatureDescriptor& info) noexcept = 0;

    virtual Nullable<shared_ptr<GraphicsPipelineState>> CreateGraphicsPipeline(
        const GraphicsPipelineStateDescriptor& desc) noexcept = 0;

    virtual Nullable<shared_ptr<SwapChain>> CreateSwapChain(
        CommandQueue* presentQueue,
        const void* nativeWindow,
        uint32_t width,
        uint32_t height,
        uint32_t backBufferCount,
        TextureFormat format,
        bool enableSync) noexcept = 0;

    virtual Nullable<shared_ptr<Buffer>> CreateBuffer(
        uint64_t size,
        ResourceType type,
        ResourceMemoryUsage usage,
        ResourceStates initState,
        ResourceHints tips,
        std::string_view name = {}) noexcept = 0;

    virtual Nullable<shared_ptr<Texture>> CreateTexture(
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
        std::string_view name = {}) noexcept = 0;

    virtual Nullable<shared_ptr<Texture>> CreateTexture(const TextureCreateDescriptor& desc) noexcept = 0;

    virtual Nullable<shared_ptr<ResourceView>> CreateBufferView(
        Buffer* buffer,
        ResourceType type,
        TextureFormat format,
        uint64_t offset,
        uint32_t count,
        uint32_t stride) noexcept = 0;

    virtual Nullable<shared_ptr<ResourceView>> CreateTextureView(
        Texture* texture,
        ResourceType type,
        TextureFormat format,
        TextureViewDimension dim,
        uint32_t baseArrayLayer,
        uint32_t arrayLayerCount,
        uint32_t baseMipLevel,
        uint32_t mipLevelCount) noexcept = 0;

    virtual Nullable<shared_ptr<DescriptorSet>> CreateDescriptorSet(const DescriptorSetElementInfo& info) noexcept = 0;

    virtual Nullable<shared_ptr<Sampler>> CreateSampler(const SamplerDescriptor& desc) noexcept = 0;

    virtual uint64_t GetUploadBufferNeedSize(Resource* copyDst, uint32_t mipLevel, uint32_t arrayLayer, uint32_t layerCount) const noexcept = 0;

    virtual void CopyDataToUploadBuffer(
        Resource* dst,
        const void* src,
        size_t srcSize,
        uint32_t mipLevel,
        uint32_t arrayLayer,
        uint32_t layerCount) const noexcept = 0;
};

Nullable<shared_ptr<Device>> CreateDevice(const DeviceDescriptor& desc);

void GlobalInitGraphics(std::span<BackendInitDescriptor> desc);

void GlobalTerminateGraphics();

}  // namespace radray::render
