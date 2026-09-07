#pragma once

#include <radray/runtime/render_framework/render_pipeline.h>

namespace radray {

class RenderSystem;
using RgComponentHandle = RgHandle<struct RgComponentTag>;
struct FrameGraphOutputDesc {
    RenderOutputId Output;
    render::TextureDescriptor Desc;
};

/// Frame-local component container. Declare components, connect typed ports, then expand
/// all components and freeze one graph. Registration order does not define GPU dependencies.
class FrameGraph {
public:
    FrameGraph(RenderPipelineContext& context, RenderGraph& graph) : Context(context), Graph(graph) {}
    RgComponentHandle AddComponent(std::string_view name, RenderGraphComponent& component, std::span<const FrameGraphOutputDesc> outputs);
    RgTexturePort Input(RgComponentHandle component, RenderOutputId output) const noexcept;
    RgTexturePort Output(RgComponentHandle component, RenderOutputId output) const noexcept;
    bool Expand();
    void Recorded(RenderGraphExecutionResult result);
    RenderPipelineContext& Context;
    RenderGraph& Graph;

private:
    struct Port {
        RenderOutputId Id;
        RgTexturePort Input, Output;
    };
    struct Component {
        RenderGraphComponent* Value;
        vector<Port> Ports;
    };
    vector<Component> _components;
    bool _expanded{false};
};

/// Application-owned assembly policy; all rendering work still belongs to ordinary components.
class FrameGraphComposer {
public:
    virtual ~FrameGraphComposer() noexcept = default;
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
