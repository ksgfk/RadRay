#include <radray/render/render_pass_registry.h>

#include <algorithm>
#include <cstdint>
#include <utility>

#include <radray/hash.h>
#include <radray/logger.h>

namespace {

void AddColorAttachment(
    radray::HashCode& hash,
    const radray::render::RenderPassColorAttachmentDescriptor& attachment) noexcept {
    hash.Add(static_cast<radray::int32_t>(attachment.Format));
    hash.Add(attachment.SampleCount);
    hash.Add(static_cast<radray::int32_t>(attachment.Load));
    hash.Add(static_cast<radray::int32_t>(attachment.Store));
}

void AddDepthStencilAttachment(
    radray::HashCode& hash,
    const radray::render::RenderPassDepthStencilAttachmentDescriptor& attachment) noexcept {
    hash.Add(static_cast<radray::int32_t>(attachment.Format));
    hash.Add(attachment.SampleCount);
    hash.Add(static_cast<radray::int32_t>(attachment.DepthLoad));
    hash.Add(static_cast<radray::int32_t>(attachment.DepthStore));
    hash.Add(static_cast<radray::int32_t>(attachment.StencilLoad));
    hash.Add(static_cast<radray::int32_t>(attachment.StencilStore));
    hash.Add(attachment.ReadOnly);
}

}  // namespace

namespace std {

size_t hash<radray::render::RenderPassCacheKey>::operator()(
    const radray::render::RenderPassCacheKey& key) const noexcept {
    radray::HashCode hash;
    hash.Add(key.ColorAttachments.size());
    // 【按顺序喂入】: 下标即渲染目标槽位, 交换附件是不同的 render pass。
    for (const auto& attachment : key.ColorAttachments) {
        AddColorAttachment(hash, attachment);
    }
    hash.Add(key.DepthStencilAttachment.has_value() ? 1u : 0u);
    if (key.DepthStencilAttachment.has_value()) {
        AddDepthStencilAttachment(hash, key.DepthStencilAttachment.value());
    }
    return hash.ToHashCode();
}

size_t hash<radray::render::FramebufferCacheKey>::operator()(
    const radray::render::FramebufferCacheKey& key) const noexcept {
    radray::HashCode hash;
    // framebuffer 的身份就是这几个指针 —— 附件是 view 的裸指针, 故哈希指针值本身。
    hash.Add(reinterpret_cast<std::uintptr_t>(key.Pass));
    hash.Add(key.ColorAttachments.size());
    for (const radray::render::TextureView* view : key.ColorAttachments) {
        hash.Add(reinterpret_cast<std::uintptr_t>(view));
    }
    hash.Add(reinterpret_cast<std::uintptr_t>(key.DepthStencilAttachment));
    hash.Add(key.Width);
    hash.Add(key.Height);
    hash.Add(key.Layers);
    return hash.ToHashCode();
}

}  // namespace std

namespace radray::render {

RenderPassCacheKey RenderPassCacheKey::Build(const RenderPassDescriptor& desc) {
    RenderPassCacheKey key{};
    key.ColorAttachments.assign(desc.ColorAttachments.begin(), desc.ColorAttachments.end());
    key.DepthStencilAttachment = desc.DepthStencilAttachment;
    return key;
}

RenderPassDescriptor RenderPassCacheKey::Get() const noexcept {
    return RenderPassDescriptor{
        .ColorAttachments = ColorAttachments,
        .DepthStencilAttachment = DepthStencilAttachment};
}

FramebufferCacheKey FramebufferCacheKey::Build(const FramebufferDescriptor& desc) {
    FramebufferCacheKey key{};
    key.Pass = desc.Pass;
    key.ColorAttachments.assign(desc.ColorAttachments.begin(), desc.ColorAttachments.end());
    key.DepthStencilAttachment = desc.DepthStencilAttachment;
    key.Width = desc.Width;
    key.Height = desc.Height;
    key.Layers = desc.Layers;
    return key;
}

FramebufferDescriptor FramebufferCacheKey::Get() const noexcept {
    return FramebufferDescriptor{
        .Pass = Pass,
        .ColorAttachments = ColorAttachments,
        .DepthStencilAttachment = DepthStencilAttachment,
        .Width = Width,
        .Height = Height,
        .Layers = Layers};
}

bool FramebufferCacheKey::References(const TextureView* view) const noexcept {
    if (view == nullptr) {
        return false;
    }
    if (DepthStencilAttachment == view) {
        return true;
    }
    return std::ranges::find(ColorAttachments, view) != ColorAttachments.end();
}

RenderPassRegistry::RenderPassRegistry(Device* device) noexcept
    : _device(device) {
    RADRAY_ASSERT(_device != nullptr);
}

RenderPassRegistry::~RenderPassRegistry() noexcept {
    Clear();
}

Nullable<RenderPass*> RenderPassRegistry::GetOrCreateRenderPass(
    const RenderPassDescriptor& desc) noexcept {
    RenderPassCacheKey key = RenderPassCacheKey::Build(desc);
    if (const auto it = _passes.find(key); it != _passes.end()) {
        ++_renderPassHits;
        return it->second.get();
    }
    ++_renderPassMisses;

    auto pass = _device->CreateRenderPass(desc);
    if (!pass.HasValue()) {
        return nullptr;
    }
    const auto [it, inserted] = _passes.try_emplace(std::move(key), pass.Release());
    RADRAY_ASSERT(inserted);
    return it->second.get();
}

Nullable<Framebuffer*> RenderPassRegistry::GetOrCreateFramebuffer(
    const FramebufferDescriptor& desc) noexcept {
    FramebufferCacheKey key = FramebufferCacheKey::Build(desc);
    if (const auto it = _framebuffers.find(key); it != _framebuffers.end()) {
        ++_framebufferHits;
        return it->second.get();
    }
    ++_framebufferMisses;

    auto framebuffer = _device->CreateFramebuffer(desc);
    if (!framebuffer.HasValue()) {
        return nullptr;
    }
    const auto [it, inserted] = _framebuffers.try_emplace(std::move(key), framebuffer.Release());
    RADRAY_ASSERT(inserted);
    return it->second.get();
}

uint32_t RenderPassRegistry::RemoveFramebuffersUsing(const TextureView* attachment) noexcept {
    if (attachment == nullptr) {
        return 0;
    }
    uint32_t removed = 0;
    for (auto it = _framebuffers.begin(); it != _framebuffers.end();) {
        if (!it->first.References(attachment)) {
            ++it;
            continue;
        }
        // 【只 Destroy 不 reset】: unique_ptr 的析构会紧接着跑, 而两个后端的 Destroy 都是
        // 幂等的 (置空句柄 / 置 _valid), 故显式 Destroy 只是让释放时机与"摘除"对齐。
        it->second->Destroy();
        it = _framebuffers.erase(it);
        ++removed;
    }
    return removed;
}

void RenderPassRegistry::ClearFramebuffers() noexcept {
    for (auto& [key, framebuffer] : _framebuffers) {
        (void)key;
        framebuffer->Destroy();
    }
    _framebuffers.clear();
}

void RenderPassRegistry::Clear() noexcept {
    // 【framebuffer 必须先清】: 它引用 pass, 反序会留下一段 pass 已死而 framebuffer 还在
    // 的窗口。这里两者都由本缓存拥有, 故顺序完全可控。
    ClearFramebuffers();
    for (auto& [key, pass] : _passes) {
        (void)key;
        pass->Destroy();
    }
    _passes.clear();
}

}  // namespace radray::render
