#include <radray/runtime/render_pass_registry.h>

#include <algorithm>

namespace radray {

RenderPassRegistry::RenderPassRegistry(render::Device* device) noexcept
    : _device(device) {}

RenderPassRegistry::~RenderPassRegistry() noexcept {
    Clear();
}

Nullable<render::RenderPass*> RenderPassRegistry::GetOrCreateRenderPass(
    const render::RenderPassDescriptor& desc) noexcept {
    for (PassEntry& entry : _passes) {
        if (entry.ColorAttachments.size() == desc.ColorAttachments.size() &&
            std::equal(entry.ColorAttachments.begin(), entry.ColorAttachments.end(), desc.ColorAttachments.begin()) &&
            entry.DepthStencilAttachment == desc.DepthStencilAttachment) {
            ++_renderPassHits;
            return entry.Object.get();
        }
    }

    ++_renderPassMisses;
    if (_device == nullptr) {
        return nullptr;
    }
    auto passOpt = _device->CreateRenderPass(desc);
    if (!passOpt.HasValue()) {
        return nullptr;
    }
    auto pass = passOpt.Release();
    render::RenderPass* result = pass.get();
    _passes.push_back(PassEntry{
        .ColorAttachments = vector<render::RenderPassColorAttachmentDescriptor>{
            desc.ColorAttachments.begin(), desc.ColorAttachments.end()},
        .DepthStencilAttachment = desc.DepthStencilAttachment,
        .Object = std::move(pass)});
    return result;
}

Nullable<render::Framebuffer*> RenderPassRegistry::GetOrCreateFramebuffer(
    render::RenderPass* pass,
    std::span<render::TextureView* const> colorAttachments,
    render::TextureView* depthStencilAttachment,
    uint32_t width,
    uint32_t height,
    uint32_t layers) noexcept {
    for (FramebufferEntry& entry : _framebuffers) {
        if (entry.Pass == pass && entry.DepthStencilAttachment == depthStencilAttachment &&
            entry.Width == width && entry.Height == height && entry.Layers == layers &&
            entry.ColorAttachments.size() == colorAttachments.size() &&
            std::equal(entry.ColorAttachments.begin(), entry.ColorAttachments.end(), colorAttachments.begin())) {
            ++_framebufferHits;
            return entry.Object.get();
        }
    }

    ++_framebufferMisses;
    if (_device == nullptr) {
        return nullptr;
    }
    render::FramebufferDescriptor desc{
        .Pass = pass,
        .ColorAttachments = colorAttachments,
        .DepthStencilAttachment = depthStencilAttachment,
        .Width = width,
        .Height = height,
        .Layers = layers};
    auto framebufferOpt = _device->CreateFramebuffer(desc);
    if (!framebufferOpt.HasValue()) {
        return nullptr;
    }
    auto framebuffer = framebufferOpt.Release();
    render::Framebuffer* result = framebuffer.get();
    _framebuffers.push_back(FramebufferEntry{
        .Pass = pass,
        .ColorAttachments = vector<render::TextureView*>{colorAttachments.begin(), colorAttachments.end()},
        .DepthStencilAttachment = depthStencilAttachment,
        .Width = width,
        .Height = height,
        .Layers = layers,
        .Object = std::move(framebuffer)});
    return result;
}

void RenderPassRegistry::RemoveFramebuffersUsing(render::TextureView* attachment) noexcept {
    if (attachment == nullptr) {
        return;
    }
    std::erase_if(_framebuffers, [attachment](FramebufferEntry& entry) noexcept {
        const bool match = entry.DepthStencilAttachment == attachment ||
                           std::ranges::find(entry.ColorAttachments, attachment) != entry.ColorAttachments.end();
        if (match && entry.Object != nullptr) {
            entry.Object->Destroy();
        }
        return match;
    });
}

void RenderPassRegistry::ClearFramebuffers() noexcept {
    for (FramebufferEntry& entry : _framebuffers) {
        if (entry.Object != nullptr) {
            entry.Object->Destroy();
        }
    }
    _framebuffers.clear();
}

void RenderPassRegistry::Clear() noexcept {
    ClearFramebuffers();
    for (PassEntry& entry : _passes) {
        if (entry.Object != nullptr) {
            entry.Object->Destroy();
        }
    }
    _passes.clear();
}

}  // namespace radray
