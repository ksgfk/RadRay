#ifdef RADRAY_ENABLE_IMGUI
#include "imgui_graph_frame.h"
#include <algorithm>
#include <radray/runtime/application.h>
#include <radray/runtime/render_framework/render_graph_blit.h>

namespace radray {
void ImGuiGraphComponent::BuildGraph(RenderPipelineContext& context, RenderGraph& graph, std::span<RenderGraphOutputBinding> outputs) {
    ImGuiGraph::BuildGraph(graph, context, _system.GetGraphFrame(context.FlightIndex()), outputs, OutputImages, Images);
}
void ImGuiGraphComponent::GraphRecorded(RenderPipelineContext& context, const RenderGraph& graph, RenderGraphExecutionResult result) {
    ImGuiGraph::CompleteGraph(graph, context, _system.GetGraphFrame(context.FlightIndex()), result.Success);
}
void ImGuiFrameComposer::PrepareFrame(RenderPrepareContext& context) { _system.RequestOutputs(context.App.FlightIndex, context.Workloads); }
void ImGuiFrameComposer::Compose(FrameGraph& frame, Nullable<RenderPipeline*> pipeline) {
    auto& graph = frame.Graph;
    auto& context = frame.Context;
    const auto snapshot = _system.GetGraphFrame(context.FlightIndex());
    const auto backend = snapshot._resources.Device.GetBackend();
    vector<FrameGraphOutputDesc> descriptions;
    for (const auto& surface : context.OutputSurfaces()) {
        auto desc = surface.Desc;
        desc.Format = render::TextureFormat::RGBA16_FLOAT;
        desc.SampleCount = 1;
        desc.Hints = render::ResourceHint::None;
        desc.Usage = render::TextureUse::RenderTarget | render::TextureUse::Resource | render::TextureUse::CopySource | render::TextureUse::CopyDestination;
        descriptions.push_back({surface.Id, desc});
    }
    const auto scene = pipeline ? frame.AddComponent("Scene", *pipeline, descriptions) : RgComponentHandle{};
    const auto ui = frame.AddComponent("UI", _component, descriptions);
    _component.OutputImages.clear();
    for (const auto& output : descriptions) {
        const auto destination = context.ImportOutputTarget(graph, output.Output);
        auto canvas = graph.CreateTexture(output.Desc, "Display.Linear");
        const auto surface = std::find_if(context.OutputSurfaces().begin(), context.OutputSurfaces().end(), [&](const auto& value) { return value.Id == output.Output; });
        if (surface != context.OutputSurfaces().end() && surface->PreserveContents) {
            const auto format = surface->Desc.Format;
            const bool decode = format == render::TextureFormat::RGBA8_UNORM || format == render::TextureFormat::BGRA8_UNORM;
            canvas = AddRenderGraphBlit(graph, _renderer, backend, destination, canvas, decode);
        } else
            graph.AddRasterPass<int>("Display.Clear", [=](int&, RenderGraphRasterBuilder& builder) { builder.SetColorAttachment(0, canvas, {.Clear = {{.012f, .012f, .012f, 1}}}); }, nullptr);
        if (scene.IsValid()) {
            graph.Connect(frame.Input(scene, output.Output), canvas);
            canvas = graph.Value(frame.Output(scene, output.Output));
        }
        graph.Connect(frame.Input(ui, output.Output), canvas);
        // Explicit snapshots preserve images sampled by UI while the same display
        // storage advances. Unreferenced snapshots are removed by normal graph culling.
        for (const auto& [id, texture] : snapshot._flight.Textures) {
            (void)id;
            if (texture.Output != output.Output) continue;
            const auto image = graph.CreateTexture(output.Desc, "Display.ImageSnapshot");
            graph.AddCopyTexturePass("Display.Snapshot", canvas, image);
            _component.OutputImages.push_back({output.Output, image, ImGuiColorEncoding::Linear});
            break;
        }
        const auto finalValue = graph.Value(frame.Output(ui, output.Output));
        const auto destinationDesc = graph.GetTextureDescriptor(destination);
        const bool srgb = destinationDesc && (destinationDesc->Format == render::TextureFormat::RGBA8_UNORM_SRGB || destinationDesc->Format == render::TextureFormat::BGRA8_UNORM_SRGB);
        const auto presented = AddRenderGraphBlit(graph, _renderer, backend, finalValue, destination, false, !srgb);
        for (const auto& surface : context.OutputSurfaces())
            if (surface.Id == output.Output) graph.ExportTexture(presented, surface.RequiredFinalState);
    }
}
}  // namespace radray
#endif
