#pragma once

#include <radray/render/backend/vulkan_impl.h>
#include <radray/render/pipeline_layout_cache.h>

namespace radray::render::vulkan {

struct PipelineLayoutKeyVulkan {
    struct PushRange {
        VkShaderStageFlags Stages;
        uint32_t Offset;
        uint32_t Size;
        friend bool operator==(const PushRange&, const PushRange&) noexcept = default;
    };
    vector<DescriptorSetLayoutVulkan*> SetLayouts;
    vector<PushRange> PushRanges;
    VkPipelineLayoutCreateFlags Flags{0};
    friend bool operator==(const PipelineLayoutKeyVulkan&, const PipelineLayoutKeyVulkan&) noexcept = default;
};

struct PipelineLayoutKeyHashVulkan {
    size_t operator()(const PipelineLayoutKeyVulkan& key) const noexcept;
};

class CachedPipelineLayoutVulkan final : public CachedPipelineLayout {
public:
    CachedPipelineLayoutVulkan(PipelineLayoutCacheVulkan* cache, DeviceVulkan* device,
                               VkPipelineLayout layout, vector<IntrusivePtr<DescriptorSetLayoutVulkan>> setLayouts) noexcept;
    ~CachedPipelineLayoutVulkan() noexcept override;

    DeviceVulkan* Device;
    VkPipelineLayout Layout;
    vector<IntrusivePtr<DescriptorSetLayoutVulkan>> SetLayouts;
    const PipelineLayoutKeyVulkan* Key{nullptr};

private:
    friend void IntrusivePtrAddRef(CachedPipelineLayoutVulkan* layout) noexcept;
    friend void IntrusivePtrRelease(CachedPipelineLayoutVulkan* layout) noexcept;
    PipelineLayoutCacheVulkan* _cache;
};

class PipelineLayoutCacheVulkan final : public PipelineLayoutCache {
public:
    explicit PipelineLayoutCacheVulkan(DeviceVulkan* device) noexcept;
    ~PipelineLayoutCacheVulkan() noexcept override;

    IntrusivePtr<CachedPipelineLayoutVulkan> GetOrCreate(
        std::span<const IntrusivePtr<DescriptorSetLayoutVulkan>> setLayouts,
        std::span<const VkPushConstantRange> pushRanges, VkPipelineLayoutCreateFlags flags = 0) noexcept;
    size_t GetEntryCount() const noexcept override;

private:
    friend void IntrusivePtrRelease(CachedPipelineLayoutVulkan* layout) noexcept;
    void Evict(CachedPipelineLayout* layout) noexcept override;
    DeviceVulkan* _device;
    unordered_map<PipelineLayoutKeyVulkan, unique_ptr<CachedPipelineLayoutVulkan>, PipelineLayoutKeyHashVulkan> _layouts;
};

}  // namespace radray::render::vulkan
