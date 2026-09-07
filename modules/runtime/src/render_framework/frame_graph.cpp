#include <radray/runtime/render_framework/frame_graph.h>

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
    return Graph.GetReport().Diagnostics.empty();
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
}  // namespace radray
