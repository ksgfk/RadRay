#pragma once

#include <radray/runtime/render_framework/render_pipeline.h>

namespace radray {

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

void ComposeDefaultFrameGraph(FrameGraph& frame, Nullable<RenderPipeline*> pipeline);

}  // namespace radray
