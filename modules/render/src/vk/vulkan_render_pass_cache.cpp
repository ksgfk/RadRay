#include <radray/render/backend/vulkan_render_pass_cache.h>
#ifdef RADRAY_ENABLE_VULKAN
#include <radray/render/backend/vulkan_impl.h>
#include <radray/hash.h>
#include <radray/logger.h>
#include <radray/profiler.h>

namespace radray::render::vulkan {
RenderPassExecutionKeyVulkan RenderPassExecutionKeyVulkan::Build(const RenderPassBeginDescriptor& scope, const AttachmentSignature& signature) {
    auto key = Representative(signature);
    for (size_t i = 0; i < scope.Colors.size(); ++i) {
        key.ColorAttachments[i].Load = scope.Colors[i].Load;
        key.ColorAttachments[i].Store = scope.Colors[i].Store;
    }
    if (scope.DepthStencil) {
        const auto& depth = *scope.DepthStencil;
        auto& d = *key.DepthStencilAttachment;
        d.DepthLoad = depth.DepthLoad;
        d.DepthStore = depth.DepthStore;
        const bool stencil = d.Format == TextureFormat::D24_UNORM_S8_UINT || d.Format == TextureFormat::D32_FLOAT_S8_UINT;
        d.StencilLoad = stencil ? depth.StencilLoad : LoadAction::DontCare;
        d.StencilStore = stencil ? depth.StencilStore : StoreAction::Discard;
        d.ReadOnly = depth.ReadOnly;
    }
    return key;
}
RenderPassExecutionKeyVulkan RenderPassExecutionKeyVulkan::Representative(const AttachmentSignature& signature) {
    RenderPassExecutionKeyVulkan key;
    for (auto format : signature.Colors) key.ColorAttachments.push_back({format, signature.Samples, LoadAction::DontCare, StoreAction::Store});
    if (signature.DepthStencil) key.DepthStencilAttachment = RenderPassDepthStencilAttachmentDescriptor{*signature.DepthStencil, signature.Samples, LoadAction::DontCare, StoreAction::Store, LoadAction::DontCare, StoreAction::Discard};
    return key;
}
size_t RenderPassKeyHashVulkan::operator()(const RenderPassExecutionKeyVulkan& key) const noexcept {
    HashCode h;
    h.Add(key.ColorAttachments.size());
    for (const auto& c : key.ColorAttachments) {
        h.Add(c.Format);
        h.Add(c.SampleCount);
        h.Add(c.Load);
        h.Add(c.Store);
    }
    h.Add(key.DepthStencilAttachment.has_value());
    if (key.DepthStencilAttachment) {
        const auto& d = *key.DepthStencilAttachment;
        h.Add(d.Format);
        h.Add(d.SampleCount);
        h.Add(d.DepthLoad);
        h.Add(d.DepthStore);
        h.Add(d.StencilLoad);
        h.Add(d.StencilStore);
        h.Add(d.ReadOnly);
    }
    return h.ToHashCode();
}
size_t FramebufferKeyHashVulkan::operator()(const FramebufferKeyVulkan& key) const noexcept {
    HashCode h;
    h.Add(key.Signature.Colors.size());
    for (auto f : key.Signature.Colors) h.Add(f);
    h.Add(key.Signature.DepthStencil.has_value());
    if (key.Signature.DepthStencil) h.Add(*key.Signature.DepthStencil);
    h.Add(key.Signature.Samples);
    for (auto id : key.Views) h.Add(id);
    h.Add(key.Width);
    h.Add(key.Height);
    h.Add(key.Layers);
    return h.ToHashCode();
}
VulkanRenderPassCache::VulkanRenderPassCache(DeviceVulkan* device) noexcept : _device(device) {}
VulkanRenderPassCache::~VulkanRenderPassCache() noexcept { Clear(); }
uint64_t VulkanRenderPassCache::AllocateViewId() {
    std::lock_guard lock{_mutex};
    if (_nextViewId == UINT64_MAX) RADRAY_ABORT("Vulkan view identity exhausted");
    return _nextViewId++;
}
IntrusivePtr<NativeRenderPassVulkan> VulkanRenderPassCache::Acquire(const RenderPassExecutionKeyVulkan& key, std::string_view name) {
    std::lock_guard lock{_mutex};
    auto found = _passes.find(key);
    if (found == _passes.end()) {
        while (_passes.size() >= _budget.RenderPasses && !_idlePasses.empty()) ErasePass(_idlePasses.front());
        if (_passes.size() >= _budget.RenderPasses) return {};
        auto native = CreatePass(key);
        if (!native) return {};
        auto entry = make_unique<NativeRenderPassVulkan>();
        entry->Cache = this;
        entry->Key = key;
        entry->Native = native;
        found = _passes.emplace(key, std::move(entry)).first;
    }
    auto* entry = found->second.get();
    if (!name.empty() && entry->Name != name) {
        entry->Name = name;
        _device->SetObjectName(entry->Name, entry->Native);
    }
    if (entry->Idle) {
        _idlePasses.erase(*entry->Idle);
        entry->Idle.reset();
    }
    ++entry->Uses;
    return AdoptRef(entry);
}
IntrusivePtr<NativeFramebufferVulkan> VulkanRenderPassCache::Acquire(const FramebufferKeyVulkan& key, VkRenderPass pass, std::span<const VkImageView> views, std::string_view name) {
    std::lock_guard lock{_mutex};
    auto found = _framebuffers.find(key);
    if (found == _framebuffers.end()) {
        while (_framebuffers.size() >= _budget.Framebuffers && !_idleFramebuffers.empty()) EraseFramebuffer(_idleFramebuffers.front());
        if (_framebuffers.size() >= _budget.Framebuffers) return {};
        RADRAY_PROFILE_SCOPE_N("VulkanRenderPassCache::CreateFramebuffer");
        VkFramebufferCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        info.renderPass = pass;
        info.attachmentCount = static_cast<uint32_t>(views.size());
        info.pAttachments = views.data();
        info.width = key.Width;
        info.height = key.Height;
        info.layers = key.Layers;
        VkFramebuffer native{};
        const auto result = _device->_ftb.vkCreateFramebuffer(_device->_device, &info, _device->GetAllocationCallbacks(), &native);
        if (result != VK_SUCCESS) {
            RADRAY_ERR_LOG("vkCreateFramebuffer failed: {}", result);
            return {};
        }
        auto entry = make_unique<NativeFramebufferVulkan>();
        entry->Cache = this;
        entry->Key = key;
        entry->Native = native;
        auto* ptr = entry.get();
        found = _framebuffers.emplace(key, std::move(entry)).first;
        for (auto id : key.Views) _byView[id].insert(ptr);
    }
    auto* entry = found->second.get();
    if (!name.empty() && entry->Name != name) {
        entry->Name = name;
        _device->SetObjectName(entry->Name, entry->Native);
    }
    if (entry->Idle) {
        _idleFramebuffers.erase(*entry->Idle);
        entry->Idle.reset();
    }
    ++entry->Uses;
    return AdoptRef(entry);
}
void VulkanRenderPassCache::AddRef(NativeRenderPassVulkan* entry) noexcept {
    std::lock_guard lock{_mutex};
    ++entry->Uses;
}
void VulkanRenderPassCache::AddRef(NativeFramebufferVulkan* entry) noexcept {
    std::lock_guard lock{_mutex};
    ++entry->Uses;
}
void VulkanRenderPassCache::Release(NativeRenderPassVulkan* entry) noexcept {
    std::lock_guard lock{_mutex};
    if (!entry->Uses) RADRAY_ABORT("invalid render pass use count");
    if (--entry->Uses == 0) {
        _idlePasses.push_back(entry);
        entry->Idle = std::prev(_idlePasses.end());
    }
    TrimLocked();
}
void VulkanRenderPassCache::Release(NativeFramebufferVulkan* entry) noexcept {
    std::lock_guard lock{_mutex};
    if (!entry->Uses) RADRAY_ABORT("invalid framebuffer use count");
    if (--entry->Uses == 0) {
        _idleFramebuffers.push_back(entry);
        entry->Idle = std::prev(_idleFramebuffers.end());
    }
    TrimLocked();
}
void IntrusivePtrAddRef(NativeRenderPassVulkan* e) noexcept { e->Cache->AddRef(e); }
void IntrusivePtrRelease(NativeRenderPassVulkan* e) noexcept { e->Cache->Release(e); }
void IntrusivePtrAddRef(NativeFramebufferVulkan* e) noexcept { e->Cache->AddRef(e); }
void IntrusivePtrRelease(NativeFramebufferVulkan* e) noexcept { e->Cache->Release(e); }
void VulkanRenderPassCache::ErasePass(NativeRenderPassVulkan* entry) {
    if (entry->Uses) RADRAY_ABORT("cannot evict a pinned Vulkan render pass");
    if (entry->Idle) _idlePasses.erase(*entry->Idle);
    _device->_ftb.vkDestroyRenderPass(_device->_device, entry->Native, _device->GetAllocationCallbacks());
    _passes.erase(entry->Key);
}
void VulkanRenderPassCache::EraseFramebuffer(NativeFramebufferVulkan* entry) {
    if (entry->Uses) RADRAY_ABORT("cannot destroy a view with command framebuffer references");
    for (auto id : entry->Key.Views) {
        auto it = _byView.find(id);
        it->second.erase(entry);
        if (it->second.empty()) _byView.erase(it);
    }
    if (entry->Idle) _idleFramebuffers.erase(*entry->Idle);
    _device->_ftb.vkDestroyFramebuffer(_device->_device, entry->Native, _device->GetAllocationCallbacks());
    _framebuffers.erase(entry->Key);
}
void VulkanRenderPassCache::InvalidateView(uint64_t id) {
    std::lock_guard lock{_mutex};
    auto found = _byView.find(id);
    if (found == _byView.end()) return;
    for (auto* entry : found->second)
        if (entry->Uses) RADRAY_ABORT("Vulkan view destruction precedes command epoch completion");
    while ((found = _byView.find(id)) != _byView.end()) EraseFramebuffer(*found->second.begin());
}
void VulkanRenderPassCache::TrimLocked() {
    while (!_idleFramebuffers.empty() && (_idleFramebuffers.size() > _budget.IdleFramebuffers || _framebuffers.size() > _budget.Framebuffers)) EraseFramebuffer(_idleFramebuffers.front());
    while (!_idlePasses.empty() && (_idlePasses.size() > _budget.IdleRenderPasses || _passes.size() > _budget.RenderPasses)) ErasePass(_idlePasses.front());
}
void VulkanRenderPassCache::SetBudget(Budget budget) {
    std::lock_guard lock{_mutex};
    _budget = budget;
    TrimLocked();
}
void VulkanRenderPassCache::Trim() {
    std::lock_guard lock{_mutex};
    TrimLocked();
}
void VulkanRenderPassCache::Clear() {
    std::lock_guard lock{_mutex};
    if (_idlePasses.size() != _passes.size() || _idleFramebuffers.size() != _framebuffers.size()) RADRAY_ABORT("Vulkan device closes with command render pass/framebuffer pins");
    while (!_idleFramebuffers.empty()) EraseFramebuffer(_idleFramebuffers.front());
    while (!_idlePasses.empty()) ErasePass(_idlePasses.front());
}

VkRenderPass VulkanRenderPassCache::CreatePass(const RenderPassExecutionKeyVulkan& desc) {
    RADRAY_PROFILE_SCOPE_N("VulkanRenderPassCache::CreateRenderPass");
    if (desc.ColorAttachments.empty() && !desc.DepthStencilAttachment.has_value()) {
        RADRAY_ERR_LOG("vk render pass must have at least one attachment");
        return VK_NULL_HANDLE;
    }

    vector<VkAttachmentDescription> attachments;
    vector<VkAttachmentReference> colorReferences;
    attachments.reserve(desc.ColorAttachments.size() + (desc.DepthStencilAttachment.has_value() ? 1u : 0u));
    colorReferences.reserve(desc.ColorAttachments.size());
    for (const RenderPassColorAttachmentDescriptor& color : desc.ColorAttachments) {
        if (color.Format == TextureFormat::UNKNOWN || color.SampleCount == 0) {
            RADRAY_ERR_LOG("vk render pass has invalid color attachment format or sample count");
            return VK_NULL_HANDLE;
        }
        attachments.push_back(VkAttachmentDescription{
            .flags = 0,
            .format = MapType(color.Format),
            .samples = MapSampleCount(color.SampleCount),
            .loadOp = MapType(color.Load),
            .storeOp = MapType(color.Store),
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL});
        colorReferences.push_back(VkAttachmentReference{
            .attachment = static_cast<uint32_t>(attachments.size() - 1),
            .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL});
    }

    VkAttachmentReference depthReference{};
    if (desc.DepthStencilAttachment.has_value()) {
        const RenderPassDepthStencilAttachmentDescriptor& depth = desc.DepthStencilAttachment.value();
        if (!IsDepthStencilFormat(depth.Format) || depth.SampleCount == 0) {
            RADRAY_ERR_LOG("vk render pass has invalid depth attachment format or sample count");
            return VK_NULL_HANDLE;
        }
        const bool hasStencil = depth.Format == TextureFormat::D24_UNORM_S8_UINT ||
                                depth.Format == TextureFormat::D32_FLOAT_S8_UINT;
        if (depth.ReadOnly && (depth.DepthLoad != LoadAction::Load || depth.DepthStore != StoreAction::Store ||
                               (hasStencil && (depth.StencilLoad != LoadAction::Load || depth.StencilStore != StoreAction::Store)))) {
            RADRAY_ERR_LOG("vk read-only depth/stencil attachment must preserve its contents");
            return VK_NULL_HANDLE;
        }
        const VkImageLayout layout = depth.ReadOnly ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        attachments.push_back(VkAttachmentDescription{
            .flags = 0,
            .format = MapType(depth.Format),
            .samples = MapSampleCount(depth.SampleCount),
            .loadOp = MapType(depth.DepthLoad),
            .storeOp = MapType(depth.DepthStore),
            .stencilLoadOp = hasStencil ? MapType(depth.StencilLoad) : VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = hasStencil ? MapType(depth.StencilStore) : VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = layout,
            .finalLayout = layout});
        depthReference = VkAttachmentReference{
            .attachment = static_cast<uint32_t>(attachments.size() - 1),
            .layout = layout};
    }

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = static_cast<uint32_t>(colorReferences.size());
    subpass.pColorAttachments = colorReferences.empty() ? nullptr : colorReferences.data();
    subpass.pDepthStencilAttachment = desc.DepthStencilAttachment.has_value() ? &depthReference : nullptr;

    VkRenderPassCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    info.attachmentCount = static_cast<uint32_t>(attachments.size());
    info.pAttachments = attachments.data();
    info.subpassCount = 1;
    info.pSubpasses = &subpass;

    VkRenderPass pass = VK_NULL_HANDLE;
    if (auto vr = _device->_ftb.vkCreateRenderPass(_device->_device, &info, _device->GetAllocationCallbacks(), &pass);
        vr != VK_SUCCESS) {
        RADRAY_ERR_LOG("vkCreateRenderPass failed: {}", vr);
        return VK_NULL_HANDLE;
    }
    return pass;
}

}  // namespace radray::render::vulkan
#endif
