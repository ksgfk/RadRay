#pragma once
#ifdef RADRAY_ENABLE_VULKAN
#include <mutex>
#include <radray/intrusive_ptr.h>
#include <radray/render/backend/vulkan_helper.h>
#include <radray/render/rhi.h>

namespace radray::render::vulkan {
class DeviceVulkan;
class VulkanRenderPassCache;
struct RenderPassColorAttachmentDescriptor {
    TextureFormat Format{TextureFormat::UNKNOWN};
    uint32_t SampleCount{1};
    LoadAction Load{LoadAction::DontCare};
    StoreAction Store{StoreAction::Store};

    friend bool operator==(const RenderPassColorAttachmentDescriptor&, const RenderPassColorAttachmentDescriptor&) noexcept = default;
};

struct RenderPassDepthStencilAttachmentDescriptor {
    TextureFormat Format{TextureFormat::UNKNOWN};
    uint32_t SampleCount{1};
    LoadAction DepthLoad{};
    StoreAction DepthStore{};
    LoadAction StencilLoad{};
    StoreAction StencilStore{};
    // Read-only depth/stencil requires Load/Store and a DepthRead framebuffer view.
    bool ReadOnly{false};

    friend bool operator==(const RenderPassDepthStencilAttachmentDescriptor&, const RenderPassDepthStencilAttachmentDescriptor&) noexcept = default;
};

struct RenderPassExecutionKeyVulkan {
    vector<RenderPassColorAttachmentDescriptor> ColorAttachments;
    std::optional<RenderPassDepthStencilAttachmentDescriptor> DepthStencilAttachment;
    static RenderPassExecutionKeyVulkan Build(const RenderPassBeginDescriptor& scope, const AttachmentSignature& signature);
    static RenderPassExecutionKeyVulkan Representative(const AttachmentSignature& signature);
    friend bool operator==(const RenderPassExecutionKeyVulkan&, const RenderPassExecutionKeyVulkan&) noexcept = default;
};
struct FramebufferKeyVulkan {
    AttachmentSignature Signature;
    vector<uint64_t> Views;
    uint32_t Width, Height, Layers;
    friend bool operator==(const FramebufferKeyVulkan&, const FramebufferKeyVulkan&) noexcept = default;
};
struct RenderPassKeyHashVulkan {
    size_t operator()(const RenderPassExecutionKeyVulkan& key) const noexcept;
};
struct FramebufferKeyHashVulkan {
    size_t operator()(const FramebufferKeyVulkan& key) const noexcept;
};
struct NativeRenderPassVulkan {
    VulkanRenderPassCache* Cache;
    RenderPassExecutionKeyVulkan Key;
    VkRenderPass Native{VK_NULL_HANDLE};
    string Name;
    uint32_t Uses{0};
    std::optional<list<NativeRenderPassVulkan*>::iterator> Idle;
};
struct NativeFramebufferVulkan {
    VulkanRenderPassCache* Cache;
    FramebufferKeyVulkan Key;
    VkFramebuffer Native{VK_NULL_HANDLE};
    string Name;
    uint32_t Uses{0};
    std::optional<list<NativeFramebufferVulkan*>::iterator> Idle;
};
void IntrusivePtrAddRef(NativeRenderPassVulkan* entry) noexcept;
void IntrusivePtrRelease(NativeRenderPassVulkan* entry) noexcept;
void IntrusivePtrAddRef(NativeFramebufferVulkan* entry) noexcept;
void IntrusivePtrRelease(NativeFramebufferVulkan* entry) noexcept;

/// Owns native storage; command references count use, independently of cache residency.
class VulkanRenderPassCache {
public:
    struct Budget {
        size_t RenderPasses{256}, Framebuffers{1024};
        size_t IdleRenderPasses{128}, IdleFramebuffers{256};
    };
    explicit VulkanRenderPassCache(DeviceVulkan* device) noexcept;
    ~VulkanRenderPassCache() noexcept;
    IntrusivePtr<NativeRenderPassVulkan> Acquire(const RenderPassExecutionKeyVulkan& key, std::string_view name = {});
    IntrusivePtr<NativeFramebufferVulkan> Acquire(const FramebufferKeyVulkan& key, VkRenderPass pass, std::span<const VkImageView> views, std::string_view name = {});
    uint64_t AllocateViewId();
    /// Called before native view destruction; rejects outstanding command references.
    void InvalidateView(uint64_t id);
    void SetBudget(Budget budget);
    void Trim();
    void Clear();
    void AddRef(NativeRenderPassVulkan* entry) noexcept;
    void AddRef(NativeFramebufferVulkan* entry) noexcept;
    void Release(NativeRenderPassVulkan* entry) noexcept;
    void Release(NativeFramebufferVulkan* entry) noexcept;

private:
    VkRenderPass CreatePass(const RenderPassExecutionKeyVulkan& key);
    void ErasePass(NativeRenderPassVulkan* entry);
    void EraseFramebuffer(NativeFramebufferVulkan* entry);
    void TrimLocked();
    DeviceVulkan* _device;
    std::mutex _mutex;
    Budget _budget;
    uint64_t _nextViewId{1};
    unordered_map<RenderPassExecutionKeyVulkan, unique_ptr<NativeRenderPassVulkan>, RenderPassKeyHashVulkan> _passes;
    unordered_map<FramebufferKeyVulkan, unique_ptr<NativeFramebufferVulkan>, FramebufferKeyHashVulkan> _framebuffers;
    unordered_map<uint64_t, unordered_set<NativeFramebufferVulkan*>> _byView;
    list<NativeRenderPassVulkan*> _idlePasses;
    list<NativeFramebufferVulkan*> _idleFramebuffers;
};
}  // namespace radray::render::vulkan
#endif
