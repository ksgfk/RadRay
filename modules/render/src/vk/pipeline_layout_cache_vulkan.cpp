#include "pipeline_layout_cache_vulkan.h"

#include <radray/hash.h>

namespace radray::render::vulkan {

size_t PipelineLayoutKeyHashVulkan::operator()(const PipelineLayoutKeyVulkan& key) const noexcept {
    HashCode hash;
    hash.Add(key.Flags);
    hash.Add(key.SetLayouts.size());
    for (const auto* layout : key.SetLayouts) hash.Add(layout);
    hash.Add(key.PushRanges.size());
    for (const auto& range : key.PushRanges) {
        hash.Add(range.Stages);
        hash.Add(range.Offset);
        hash.Add(range.Size);
    }
    return hash.ToHashCode();
}

CachedPipelineLayoutVulkan::CachedPipelineLayoutVulkan(
    PipelineLayoutCacheVulkan* cache, DeviceVulkan* device, VkPipelineLayout layout,
    vector<IntrusivePtr<DescriptorSetLayoutVulkan>> setLayouts) noexcept
    : Device(device), Layout(layout), SetLayouts(std::move(setLayouts)), _cache(cache) {}

CachedPipelineLayoutVulkan::~CachedPipelineLayoutVulkan() noexcept {
    Device->_ftb.vkDestroyPipelineLayout(Device->_device, Layout, Device->GetAllocationCallbacks());
}

PipelineLayoutCacheVulkan::PipelineLayoutCacheVulkan(DeviceVulkan* device) noexcept
    : _device(device) {}

PipelineLayoutCacheVulkan::~PipelineLayoutCacheVulkan() noexcept {
    Destroy();
}

size_t PipelineLayoutCacheVulkan::GetEntryCount() const noexcept {
    return _layouts.size();
}

IntrusivePtr<CachedPipelineLayoutVulkan> PipelineLayoutCacheVulkan::GetOrCreate(
    std::span<const IntrusivePtr<DescriptorSetLayoutVulkan>> setLayouts,
    std::span<const VkPushConstantRange> pushRanges, VkPipelineLayoutCreateFlags flags) noexcept {
    if (IsClosed() || _device->_device == VK_NULL_HANDLE) return nullptr;
    PipelineLayoutKeyVulkan key;
    key.Flags = flags;
    for (const auto& setLayout : setLayouts) {
        if (!setLayout) return nullptr;
        key.SetLayouts.push_back(setLayout.Get());
    }
    for (const auto& range : pushRanges) key.PushRanges.push_back({range.stageFlags, range.offset, range.size});
    if (const auto it = _layouts.find(key); it != _layouts.end()) return RetainRef(it->second.get());

    vector<VkDescriptorSetLayout> handles;
    for (const auto& setLayout : setLayouts) handles.push_back(setLayout->Get());
    VkPipelineLayoutCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    info.flags = flags;
    info.setLayoutCount = static_cast<uint32_t>(handles.size());
    info.pSetLayouts = handles.empty() ? nullptr : handles.data();
    info.pushConstantRangeCount = static_cast<uint32_t>(pushRanges.size());
    info.pPushConstantRanges = pushRanges.empty() ? nullptr : pushRanges.data();
    VkPipelineLayout layout = VK_NULL_HANDLE;
    if (const VkResult vr = _device->_ftb.vkCreatePipelineLayout(
            _device->_device, &info, _device->GetAllocationCallbacks(), &layout);
        vr != VK_SUCCESS) {
        RADRAY_ERR_LOG("vkCreatePipelineLayout failed: {}", vr);
        return nullptr;
    }
    const auto [it, inserted] = _layouts.try_emplace(
        std::move(key), make_unique<CachedPipelineLayoutVulkan>(
                            this, _device, layout,
                            vector<IntrusivePtr<DescriptorSetLayoutVulkan>>(setLayouts.begin(), setLayouts.end())));
    RADRAY_ASSERT(inserted);
    it->second->Key = &it->first;
    return RetainRef(it->second.get());
}

void PipelineLayoutCacheVulkan::Evict(CachedPipelineLayout* layout) noexcept {
    auto* entry = static_cast<CachedPipelineLayoutVulkan*>(layout);
    const auto it = _layouts.find(*entry->Key);
    RADRAY_ASSERT(it != _layouts.end() && it->second.get() == entry && entry->GetRefCount() == 0);
    _layouts.erase(it);
}

}  // namespace radray::render::vulkan
