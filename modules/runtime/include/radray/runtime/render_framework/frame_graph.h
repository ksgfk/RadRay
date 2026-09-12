#pragma once

#include <radray/runtime/render_framework/render_pipeline.h>

namespace radray {

class RenderSystem;
using RgComponentHandle = RgHandle<struct RgComponentTag>;
struct FrameGraphOutputDesc {
    RenderOutputId Output;
    render::TextureDescriptor Desc;
};
struct FrameGraphComponentPort {
    RenderOutputId Id;
    RgTexturePort Input, Output;
};

/// Frame-local component container. Declare components, connect typed ports, then expand
/// all components and freeze one graph. Registration order does not define GPU dependencies.
class FrameGraph {
public:
    FrameGraph(RenderPipelineContext& context, RenderGraph& graph) : Context(context), Graph(graph) {}
    RgComponentHandle AddComponent(std::string_view name, RenderGraphComponent& component, std::span<const FrameGraphOutputDesc> outputs);
    /// Registers this frame's component using ports already mapped from a template instance.
    RgComponentHandle AddComponent(RenderGraphComponent& component, std::span<const FrameGraphComponentPort> ports);
    RgTexturePort Input(RgComponentHandle component, RenderOutputId output) const noexcept;
    RgTexturePort Output(RgComponentHandle component, RenderOutputId output) const noexcept;
    bool Expand();
    void Recorded(RenderGraphExecutionResult result);
    RenderPipelineContext& Context;
    RenderGraph& Graph;

private:
    struct Component {
        RenderGraphComponent* Value;
        vector<FrameGraphComponentPort> Ports;
    };
    vector<Component> _components;
    bool _expanded{false};
};

/// Device-local default assembly declarations, shared across flights through the plan cache.
/// Entries own logical topology only; component pointers and native bindings remain frame-local.
class FrameGraphTemplateCache {
public:
    explicit FrameGraphTemplateCache(size_t capacity = 4);
    ~FrameGraphTemplateCache();
    FrameGraphTemplateCache(const FrameGraphTemplateCache&) = delete;
    FrameGraphTemplateCache& operator=(const FrameGraphTemplateCache&) = delete;
    size_t Size() const noexcept;
    uint64_t BuildCount() const noexcept;
    void Clear() noexcept;
    vector<RenderGraphOutputBinding> Compose(FrameGraph& frame, Nullable<RenderPipeline*> pipeline,
                                             std::span<RenderGraphComponent* const> overlays, Nullable<RenderSystem*> renderer);

private:
    struct Impl;
    unique_ptr<Impl> _impl;
};

/// Application-owned assembly policy; all rendering work still belongs to ordinary components.
class FrameGraphComposer {
public:
    virtual ~FrameGraphComposer() noexcept = default;
    virtual void CollectScenePolicies(RenderPrepareContext&) {}
    virtual void PrepareFrame(RenderPrepareContext&) {}
    virtual void Compose(FrameGraph& frame, Nullable<RenderPipeline*> pipeline) = 0;
};

/// Default assembly. Without overlays each output is rendered directly by the scene pipeline.
/// With overlays each output uses a display-linear RGBA16_FLOAT canvas: preserved contents are
/// loaded through a blit (UNORM decoded), the scene renders into it, overlays follow in
/// registration order, and the final blit encodes for the output format (non-sRGB UNORM outputs
/// receive explicit sRGB encoding, sRGB attachments encode in hardware).
void ComposeDefaultFrameGraph(FrameGraph& frame, Nullable<RenderPipeline*> pipeline);
/// Returns the exported value of every output so wrapping composers can observe the final frame.
vector<RenderGraphOutputBinding> ComposeDefaultFrameGraph(FrameGraph& frame, Nullable<RenderPipeline*> pipeline,
                                                          std::span<RenderGraphComponent* const> overlays, RenderSystem& renderer);

}  // namespace radray
