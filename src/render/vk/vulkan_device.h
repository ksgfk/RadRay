#pragma once

#include <radray/render/device.h>

#include "vulkan_helper.h"

namespace radray::render::vulkan {

class DeviceVulkan : public Device {
public:
    DeviceVulkan() = default;
    ~DeviceVulkan() noexcept override;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

    Backend GetBackend() noexcept override { return Backend::Vulkan; }

    Nullable<CommandQueue> GetCommandQueue(QueueType type, uint32_t slot) noexcept override;

    Nullable<radray::shared_ptr<Fence>> CreateFence() noexcept override;

    Nullable<radray::shared_ptr<Shader>> CreateShader(
        std::span<const byte> blob,
        ShaderBlobCategory category,
        ShaderStage stage,
        std::string_view entryPoint,
        std::string_view name) noexcept override;

    Nullable<radray::shared_ptr<RootSignature>> CreateRootSignature(const RootSignatureDescriptor& info) noexcept override;

    Nullable<radray::shared_ptr<GraphicsPipelineState>> CreateGraphicsPipeline(
        const GraphicsPipelineStateDescriptor& desc) noexcept override;

    Nullable<radray::shared_ptr<SwapChain>> CreateSwapChain(
        CommandQueue* presentQueue,
        const void* nativeWindow,
        uint32_t width,
        uint32_t height,
        uint32_t backBufferCount,
        TextureFormat format,
        bool enableSync) noexcept override;

    Nullable<radray::shared_ptr<Buffer>> CreateBuffer(
        uint64_t size,
        ResourceType type,
        ResourceUsage usage,
        ResourceStates initState,
        ResourceMemoryTips tips,
        std::string_view name = {}) noexcept override;

    Nullable<radray::shared_ptr<Texture>> CreateTexture(
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
        std::string_view name = {}) noexcept override;

    Nullable<radray::shared_ptr<ResourceView>> CreateBufferView(
        Buffer* buffer,
        ResourceType type,
        TextureFormat format,
        uint64_t offset,
        uint32_t count,
        uint32_t stride) noexcept override;

    Nullable<radray::shared_ptr<ResourceView>> CreateTextureView(
        Texture* texture,
        ResourceType type,
        TextureFormat format,
        TextureDimension dim,
        uint32_t baseArrayLayer,
        uint32_t arrayLayerCount,
        uint32_t baseMipLevel,
        uint32_t mipLevelCount) noexcept override;

    Nullable<radray::shared_ptr<DescriptorSet>> CreateDescriptorSet(const DescriptorSetElementInfo& info) noexcept override;

    Nullable<radray::shared_ptr<Sampler>> CreateSampler(const SamplerDescriptor& desc) noexcept override;

    uint64_t GetUploadBufferNeedSize(Resource* copyDst, uint32_t mipLevel, uint32_t arrayLayer, uint32_t layerCount) const noexcept override;

    void CopyDataToUploadBuffer(
        Resource* dst,
        const void* src,
        size_t srcSize,
        uint32_t mipLevel,
        uint32_t arrayLayer,
        uint32_t layerCount) const noexcept override;

public:
    VkPhysicalDevice _physicalDevice;
    VkDevice _device;

    VolkDeviceTable _vtb;
};

Nullable<radray::shared_ptr<DeviceVulkan>> CreateDevice(const VulkanDeviceDescriptor& desc);

bool GlobalInitVulkan(std::span<BackendInitDescriptor> desc);

void GlobalTerminateVulkan();

}  // namespace radray::render::vulkan
