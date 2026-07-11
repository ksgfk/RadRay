#pragma once

#include <span>

#include <radray/nullable.h>
#include <radray/render/common.h>
#include <radray/types.h>

namespace radray {

class RenderPassRegistry {
public:
    explicit RenderPassRegistry(render::Device* device) noexcept;
    ~RenderPassRegistry() noexcept;
    RenderPassRegistry(const RenderPassRegistry&) = delete;
    RenderPassRegistry& operator=(const RenderPassRegistry&) = delete;

    Nullable<render::RenderPass*> GetOrCreateRenderPass(
        const render::RenderPassDescriptor& desc) noexcept;

    Nullable<render::Framebuffer*> GetOrCreateFramebuffer(
        render::RenderPass* pass,
        std::span<render::TextureView* const> colorAttachments,
        render::TextureView* depthStencilAttachment,
        uint32_t width,
        uint32_t height,
        uint32_t layers = 1) noexcept;

    void RemoveFramebuffersUsing(render::TextureView* attachment) noexcept;
    void ClearFramebuffers() noexcept;
    void Clear() noexcept;

    uint32_t GetRenderPassCount() const noexcept { return static_cast<uint32_t>(_passes.size()); }
    uint32_t GetFramebufferCount() const noexcept { return static_cast<uint32_t>(_framebuffers.size()); }
    uint64_t GetRenderPassHitCount() const noexcept { return _renderPassHits; }
    uint64_t GetRenderPassMissCount() const noexcept { return _renderPassMisses; }
    uint64_t GetFramebufferHitCount() const noexcept { return _framebufferHits; }
    uint64_t GetFramebufferMissCount() const noexcept { return _framebufferMisses; }

private:
    struct PassEntry {
        vector<render::RenderPassColorAttachmentDescriptor> ColorAttachments;
        std::optional<render::RenderPassDepthStencilAttachmentDescriptor> DepthStencilAttachment;
        unique_ptr<render::RenderPass> Object;
    };

    struct FramebufferEntry {
        render::RenderPass* Pass{nullptr};
        vector<render::TextureView*> ColorAttachments;
        render::TextureView* DepthStencilAttachment{nullptr};
        uint32_t Width{0};
        uint32_t Height{0};
        uint32_t Layers{1};
        unique_ptr<render::Framebuffer> Object;
    };

    render::Device* _device{nullptr};
    vector<PassEntry> _passes;
    vector<FramebufferEntry> _framebuffers;
    uint64_t _renderPassHits{0};
    uint64_t _renderPassMisses{0};
    uint64_t _framebufferHits{0};
    uint64_t _framebufferMisses{0};
};

}  // namespace radray
