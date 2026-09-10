#include <radray/runtime/render_framework/frame_graph.h>

#include <algorithm>

#include <radray/profiler.h>
#include <radray/runtime/render_framework/render_graph_blit.h>

namespace radray {
Nullable<RenderGraphOutputBinding*> FindGraphOutput(std::span<RenderGraphOutputBinding> outputs, RenderOutputId id) noexcept {
    for (auto& output : outputs)
        if (output.Output == id) return &output;
    return nullptr;
}
RgComponentHandle FrameGraph::AddComponent(std::string_view name, RenderGraphComponent& component, std::span<const FrameGraphOutputDesc> outputs) {
    if (_expanded) {
        Graph.AddDiagnostic("ComponentsFrozen", "Components cannot be added after expansion");
        return {};
    }
    Component entry{&component, {}};
    for (const auto& output : outputs) {
        for (const auto& existing : entry.Ports)
            if (existing.Id == output.Output) {
                Graph.AddDiagnostic("ComponentOutput", "Component output identifiers must be unique");
                return {};
            }
        entry.Ports.push_back({output.Output,
                               Graph.DeclareTexturePort(output.Desc, fmt::format("{}.Input.{}", name, output.Output.Value)),
                               Graph.DeclareTexturePort(output.Desc, fmt::format("{}.Output.{}", name, output.Output.Value))});
    }
    const auto index = static_cast<uint32_t>(_components.size());
    _components.push_back(std::move(entry));
    return {index, Graph.GetGeneration()};
}
RgTexturePort FrameGraph::Input(RgComponentHandle component, RenderOutputId output) const noexcept {
    if (component.Generation != Graph.GetGeneration() || component.Index >= _components.size()) return {};
    for (const auto& port : _components[component.Index].Ports)
        if (port.Id == output) return port.Input;
    return {};
}
RgTexturePort FrameGraph::Output(RgComponentHandle component, RenderOutputId output) const noexcept {
    if (component.Generation != Graph.GetGeneration() || component.Index >= _components.size()) return {};
    for (const auto& port : _components[component.Index].Ports)
        if (port.Id == output) return port.Output;
    return {};
}
bool FrameGraph::Expand() {
    RADRAY_PROFILE_SCOPE_N("FrameGraph::Expand");
    if (_expanded) {
        Graph.AddDiagnostic("ComponentsFrozen", "Components expand only once");
        return false;
    }
    _expanded = true;
    for (auto& component : _components) {
        vector<RenderGraphOutputBinding> outputs;
        for (const auto& port : component.Ports) outputs.push_back({port.Id, Graph.Value(port.Input)});
        component.Value->BuildGraph(Context, Graph, outputs);
        for (const auto& port : component.Ports) {
            const auto value = FindGraphOutput(outputs, port.Id);
            if (!value || !value->Texture.IsValid())
                Graph.AddDiagnostic("ComponentOutput", "Component did not produce a valid output value");
            else
                Graph.Connect(port.Output, value->Texture);
        }
    }
    return !Graph.HasFailed();
}
void FrameGraph::Recorded(RenderGraphExecutionResult result) {
    for (auto& component : _components) component.Value->GraphRecorded(Context, Graph, result);
}
void ComposeDefaultFrameGraph(FrameGraph& frame, Nullable<RenderPipeline*> pipeline) {
    vector<FrameGraphOutputDesc> descriptions;
    for (const auto& surface : frame.Context.OutputSurfaces()) descriptions.push_back({surface.Id, surface.Desc});
    const auto renderer = pipeline ? frame.AddComponent("Renderer", *pipeline, descriptions) : RgComponentHandle{};
    for (const auto& surface : frame.Context.OutputSurfaces()) {
        auto target = frame.Context.ImportOutputTarget(frame.Graph, surface.Id);
        if (!surface.PreserveContents) {
            target = frame.Graph.NextVersion(target);
            frame.Graph.AddRasterPass<int>("Output.Clear", [=](int&, RenderGraphRasterBuilder& builder) { builder.SetColorAttachment(0, target, {.Clear = {{.08f, .10f, .14f, 1}}}); }, nullptr);
        }
        if (renderer.IsValid()) {
            frame.Graph.Connect(frame.Input(renderer, surface.Id), target);
            target = frame.Graph.Value(frame.Output(renderer, surface.Id));
        }
        frame.Graph.ExportTexture(target, surface.RequiredFinalState);
    }
}
vector<RenderGraphOutputBinding> ComposeDefaultFrameGraph(FrameGraph& frame, Nullable<RenderPipeline*> pipeline,
                                                          std::span<RenderGraphComponent* const> overlays, RenderSystem& renderer) {
    vector<RenderGraphOutputBinding> exported;
    if (overlays.empty()) {
        ComposeDefaultFrameGraph(frame, pipeline);
        return exported;
    }
    auto& graph = frame.Graph;
    auto& context = frame.Context;
    const auto backend = context.Backend();
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
    vector<RgComponentHandle> overlayHandles;
    for (size_t index = 0; index < overlays.size(); ++index)
        overlayHandles.push_back(frame.AddComponent(fmt::format("Overlay.{}", index), *overlays[index], descriptions));
    for (const auto& output : descriptions) {
        const auto surface = std::find_if(context.OutputSurfaces().begin(), context.OutputSurfaces().end(), [&](const auto& value) { return value.Id == output.Output; });
        if (surface == context.OutputSurfaces().end()) continue;
        const auto destination = context.ImportOutputTarget(graph, output.Output);
        auto canvas = graph.CreateTexture(output.Desc, "Display.Linear");
        const auto format = surface->Desc.Format;
        const bool srgb = format == render::TextureFormat::RGBA8_UNORM_SRGB || format == render::TextureFormat::BGRA8_UNORM_SRGB;
        const bool unorm = format == render::TextureFormat::RGBA8_UNORM || format == render::TextureFormat::BGRA8_UNORM;
        if (surface->PreserveContents)
            canvas = AddRenderGraphBlit(graph, renderer, backend, destination, canvas, unorm);
        else
            graph.AddRasterPass<int>("Display.Clear", [=](int&, RenderGraphRasterBuilder& builder) { builder.SetColorAttachment(0, canvas, {.Clear = {{.012f, .012f, .012f, 1}}}); }, nullptr);
        if (scene.IsValid()) {
            graph.Connect(frame.Input(scene, output.Output), canvas);
            canvas = graph.Value(frame.Output(scene, output.Output));
        }
        for (const auto& overlay : overlayHandles) {
            graph.Connect(frame.Input(overlay, output.Output), canvas);
            canvas = graph.Value(frame.Output(overlay, output.Output));
        }
        const auto presented = AddRenderGraphBlit(graph, renderer, backend, canvas, destination, false, !srgb);
        graph.ExportTexture(presented, surface->RequiredFinalState);
        exported.push_back({output.Output, presented});
    }
    return exported;
}
}  // namespace radray
