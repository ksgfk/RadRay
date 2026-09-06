#include <radray/runtime/forward_pipeline/forward_graph.h>

#include <algorithm>
#include <radray/runtime/render_framework/viewport.h>

namespace radray {
namespace {

struct ForwardGraphPassData {
    struct ViewData {
        ResolvedRenderView View;
        const RendererList* List{nullptr};
        std::optional<RendererListPassBindings> Bindings;
    };

    render::RenderBackend Backend{render::RenderBackend::MAX_COUNT};
    vector<ViewData> Views;
    DrawExecutionStats* Execution{nullptr};
};

void ExecuteForwardGraphPass(
    const ForwardGraphPassData& data, RenderGraphRasterContext& context) {
    for (const ForwardGraphPassData::ViewData& view : data.Views) {
        if (view.List == nullptr) continue;
        context.Encoder().SetViewport(MakeViewport(
            data.Backend, static_cast<float>(view.View.ViewRect.X),
            static_cast<float>(view.View.ViewRect.Y),
            static_cast<float>(view.View.ViewRect.Width),
            static_cast<float>(view.View.ViewRect.Height)));
        context.Encoder().SetScissor(view.View.ScissorRect);
        if (view.Bindings)
            SubmitRendererList(*view.List, context, context.PassState(), *view.Bindings, *data.Execution);
        else
            SubmitRendererList(*view.List, context, context.PassState(), *data.Execution);
    }
}

}  // namespace

ForwardGraphStageOutput ForwardGraph::BuildGraph(
    RenderGraph& graph, ForwardGraphStage stage,
    const ForwardGraphStageInputs& inputs) {
    ForwardGraphStageOutput result{
        .Color = inputs.Color,
        .Depth = inputs.Depth,
        .Pass = {},
        .Success = false};
    if (!EnumContains(stage) || !EnumContains(inputs.Backend) ||
        !inputs.Depth.IsValid() || inputs.Execution == nullptr ||
        (stage != ForwardGraphStage::Depth && !inputs.Color.IsValid())) {
        return result;
    }
    for (const ForwardGraphView& view : inputs.Views) {
        if (view.List == nullptr) return result;
    }

    const bool hasCommands = std::any_of(
        inputs.Views.begin(), inputs.Views.end(),
        [](const ForwardGraphView& view) { return !view.List->Commands.empty(); });
    if (!hasCommands && stage != ForwardGraphStage::Opaque && !inputs.PreserveEmptyPass) {
        result.Success = true;
        return result;
    }

    bool setupSuccess = true;
    result.Pass = graph.AddRasterPass<ForwardGraphPassData>(
        inputs.Name,
        [&](ForwardGraphPassData& data, RenderGraphRasterBuilder& builder) {
            data.Backend = inputs.Backend;
            data.Execution = inputs.Execution;
            data.Views.reserve(inputs.Views.size());
            for (const ForwardGraphView& view : inputs.Views) {
                ForwardGraphPassData::ViewData next{view.View, view.List, {}};
                if (!view.Parameters.empty()) {
                    next.Bindings = RendererListPassBindings::Create(builder, *view.List, view.Parameters);
                    if (!next.Bindings) setupSuccess = false;
                }
                data.Views.push_back(std::move(next));
            }
            if (stage != ForwardGraphStage::Depth &&
                !builder.SetColorAttachment(0, inputs.Color, inputs.ColorAttachment).IsValid()) {
                setupSuccess = false;
            }
            RgDepthAttachmentDesc depth = inputs.DepthAttachment;
            depth.ReadOnly = depth.ReadOnly || stage == ForwardGraphStage::Transparent;
            for (uint32_t i = 0; i < inputs.AuxiliaryColors.size(); ++i)
                if (!builder.SetColorAttachment(i + 1, inputs.AuxiliaryColors[i]).IsValid()) setupSuccess = false;
            if (!builder.SetDepthAttachment(inputs.Depth, depth).IsValid()) setupSuccess = false;
        },
        ExecuteForwardGraphPass);
    result.Success = result.Pass.IsValid() && setupSuccess;
    return result;
}

}  // namespace radray
