#pragma once

#include <array>

#include <radray/render/device.h>

#include "vulkan_helper.h"
#include "vulkan_cmd_queue.h"

namespace radray::render::vulkan {

class DeviceVulkan : public Device {
public:
    VkInstance _instance = VK_NULL_HANDLE;
    VkPhysicalDevice _physicalDevice = VK_NULL_HANDLE;
    VkDevice _device = VK_NULL_HANDLE;
    VmaAllocator _alloc = VK_NULL_HANDLE;
    std::array<vector<unique_ptr<QueueVulkan>>, (size_t)QueueType::MAX_COUNT> _queues;

    VolkDeviceTable _vtb;

public:
    DeviceVulkan() = default;
    ~DeviceVulkan() noexcept override;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

    Backend GetBackend() noexcept override { return Backend::Vulkan; }

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

    template <typename F, typename... Args>
    constexpr auto CallVk(F FTbVk::* member, Args&&... args) const -> decltype(((this->_vtb).*member)(std::declval<Args>()...)) {
        return ((this->_vtb).*member)(std::forward<Args>(args)...);
    }

    template <typename F, typename... Args>
    requires std::is_same_v<std::tuple_element_t<0, typename FunctionTraits<F>::ArgsTuple>, VkDevice>
    constexpr auto CallVk(F FTbVk::* member, Args&&... args) const -> decltype(((this->_vtb).*member)(this->_device, std::declval<Args>()...)) {
        return ((this->_vtb).*member)(this->_device, std::forward<Args>(args)...);
    }

    const VkAllocationCallbacks* GetAllocationCallbacks() const noexcept;

    Nullable<shared_ptr<SemaphoreVulkan>> CreateSemaphoreVk(VkSemaphoreCreateFlags flags) noexcept;

    Nullable<shared_ptr<FenceVulkan>> CreateFenceVk(VkFenceCreateFlags flags) noexcept;
};

Nullable<shared_ptr<DeviceVulkan>> CreateDevice(const VulkanDeviceDescriptor& desc);

bool GlobalInitVulkan(std::span<BackendInitDescriptor> desc);

void GlobalTerminateVulkan();

}  // namespace radray::render::vulkan
