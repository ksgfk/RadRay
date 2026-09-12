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
    vector<FrameGraphComponentPort> ports;
    ports.reserve(outputs.size());
    for (const auto& output : outputs) {
        for (const auto& existing : ports)
            if (existing.Id == output.Output) {
                Graph.AddDiagnostic("ComponentOutput", "Component output identifiers must be unique");
                return {};
            }
        ports.push_back({output.Output,
                         Graph.DeclareTexturePort(output.Desc, fmt::format("{}.Input.{}", name, output.Output.Value)),
                         Graph.DeclareTexturePort(output.Desc, fmt::format("{}.Output.{}", name, output.Output.Value))});
    }
    return AddComponent(component, ports);
}
RgComponentHandle FrameGraph::AddComponent(RenderGraphComponent& component, std::span<const FrameGraphComponentPort> ports) {
    if (_expanded) {
        Graph.AddDiagnostic("ComponentsFrozen", "Components cannot be added after expansion");
        return {};
    }
    for (size_t index = 0; index < ports.size(); ++index) {
        const auto& port = ports[index];
        if (!port.Input.IsValid() || !port.Output.IsValid() || port.Input.Generation != Graph.GetGeneration() || port.Output.Generation != Graph.GetGeneration()) {
            Graph.AddDiagnostic("ComponentOutput", "Component ports must belong to this frame graph");
            return {};
        }
        for (size_t previous = 0; previous < index; ++previous)
            if (ports[previous].Id == port.Id) {
                Graph.AddDiagnostic("ComponentOutput", "Component output identifiers must be unique");
                return {};
            }
    }
    const auto index = static_cast<uint32_t>(_components.size());
    _components.push_back({&component, {ports.begin(), ports.end()}});
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

namespace {
struct ClearFrame {};
struct ClearRecipe {};
struct CompositionOutputKey {
    RenderOutputId Id;
    TexturePoolKey Texture;
    render::TextureStates FinalState;
    bool PreserveContents;
};
struct CompositionEntry {
    vector<CompositionOutputKey> Outputs;
    vector<vector<FrameGraphComponentPort>> Components;
    vector<RgTextureValue> ExternalSlots;
    vector<RenderGraphOutputBinding> Exports;
    vector<RenderGraphBlitTemplatePass> Blits;
    RgTemplateSlot<ClearFrame> Clear;
    shared_ptr<const RenderGraphTemplate> Template;
    RenderGraphRuntimeOptions Runtime;
    size_t OverlayCount{0};
    bool HasPipeline{false};
    uint64_t LastUsed{0};
};
bool Matches(const CompositionEntry& entry, const FrameGraph& frame, bool pipeline, size_t overlays) {
    const auto surfaces = frame.Context.OutputSurfaces();
    if (entry.HasPipeline != pipeline || entry.OverlayCount != overlays || entry.Runtime != frame.Context.GetRuntimeOptions() || entry.Outputs.size() != surfaces.size()) return false;
    for (size_t index = 0; index < surfaces.size(); ++index) {
        const auto& key = entry.Outputs[index];
        const auto& surface = surfaces[index];
        if (key.Id != surface.Id || key.Texture != TexturePoolKey{surface.Desc} || key.FinalState != surface.RequiredFinalState || key.PreserveContents != surface.PreserveContents) return false;
    }
    return true;
}
void DeclareClear(RenderGraph& graph, CompositionEntry& entry, std::string_view name, RgTextureValue target, render::ColorClearValue color) {
    if (!entry.Clear.IsValid()) entry.Clear = graph.DeclareTemplateSlot<ClearFrame>();
    graph.AddTemplateRasterPass<ClearRecipe>(name, entry.Clear, [=](ClearRecipe&, RenderGraphRasterBuilder& pass) { pass.SetColorAttachment(0, target, {.Clear = color}); }, +[](const ClearRecipe&, ClearFrame&, RenderGraphPrepareContext&) { return true; }, +[](const ClearRecipe&, const ClearFrame&, RenderGraphRasterContext&) {});
}
CompositionEntry BuildComposition(FrameGraph& frame, bool hasPipeline, size_t overlays) {
    RADRAY_PROFILE_SCOPE_N("FrameGraph::BuildDefaultTemplate");
    auto builder = frame.Graph.CreateTemplateBuilder("Default frame composition");
    CompositionEntry entry;
    entry.Runtime = frame.Context.GetRuntimeOptions();
    entry.HasPipeline = hasPipeline;
    entry.OverlayCount = overlays;
    vector<FrameGraphOutputDesc> descriptions;
    for (const auto& surface : frame.Context.OutputSurfaces()) {
        entry.Outputs.push_back({surface.Id, {surface.Desc}, surface.RequiredFinalState, surface.PreserveContents});
        auto desc = surface.Desc;
        if (overlays != 0) {
            desc.Format = render::TextureFormat::RGBA16_FLOAT;
            desc.SampleCount = 1;
            desc.Hints = render::ResourceHint::None;
            desc.Usage = render::TextureUse::RenderTarget | render::TextureUse::Resource | render::TextureUse::CopySource | render::TextureUse::CopyDestination;
        }
        descriptions.push_back({surface.Id, desc});
    }
    const size_t componentCount = size_t(hasPipeline) + overlays;
    for (size_t index = 0; index < componentCount; ++index) {
        const auto name = hasPipeline && index == 0 ? string{overlays == 0 ? "Renderer" : "Scene"} : fmt::format("Overlay.{}", index - size_t(hasPipeline));
        vector<FrameGraphComponentPort> ports;
        for (const auto& output : descriptions)
            ports.push_back({output.Output,
                             builder.DeclareTexturePort(output.Desc, fmt::format("{}.Input.{}", name, output.Output.Value)),
                             builder.DeclareTexturePort(output.Desc, fmt::format("{}.Output.{}", name, output.Output.Value))});
        entry.Components.push_back(std::move(ports));
    }
    for (size_t index = 0; index < entry.Outputs.size(); ++index) {
        const auto& output = entry.Outputs[index];
        const auto destination = builder.DeclareExternalTexture(output.Texture.Desc, fmt::format("Output.{}", output.Id.Value), RenderGraphExternalAccess::ObservableOutput);
        entry.ExternalSlots.push_back(destination);
        auto target = destination;
        if (overlays == 0) {
            if (!output.PreserveContents) {
                target = builder.NextVersion(target);
                DeclareClear(builder, entry, "Output.Clear", target, {{.08f, .10f, .14f, 1}});
            }
        } else {
            target = builder.CreateTexture(descriptions[index].Desc, "Display.Linear");
            if (output.PreserveContents) {
                const auto format = output.Texture.Desc.Format;
                const bool unorm = format == render::TextureFormat::RGBA8_UNORM || format == render::TextureFormat::BGRA8_UNORM;
                entry.Blits.push_back(DeclareRenderGraphBlit(builder, destination, target, unorm));
                target = entry.Blits.back().Output;
            } else {
                DeclareClear(builder, entry, "Display.Clear", target, {{.012f, .012f, .012f, 1}});
            }
        }
        for (const auto& component : entry.Components) {
            builder.Connect(component[index].Input, target);
            target = builder.Value(component[index].Output);
        }
        if (overlays != 0) {
            const auto format = output.Texture.Desc.Format;
            const bool srgb = format == render::TextureFormat::RGBA8_UNORM_SRGB || format == render::TextureFormat::BGRA8_UNORM_SRGB;
            entry.Blits.push_back(DeclareRenderGraphBlit(builder, target, destination, false, !srgb));
            target = entry.Blits.back().Output;
        }
        builder.ExportTexture(target, output.FinalState);
        entry.Exports.push_back({output.Id, target});
    }
    entry.Template = builder.FreezeTemplate();
    return entry;
}
}  // namespace

struct FrameGraphTemplateCache::Impl {
    explicit Impl(size_t capacity) : Capacity(std::max(size_t{1}, capacity)) { Entries.reserve(Capacity); }
    vector<CompositionEntry> Entries;
    size_t Capacity;
    uint64_t Clock{0}, Builds{0};
};
FrameGraphTemplateCache::FrameGraphTemplateCache(size_t capacity) : _impl(make_unique<Impl>(capacity)) {}
FrameGraphTemplateCache::~FrameGraphTemplateCache() = default;
size_t FrameGraphTemplateCache::Size() const noexcept { return _impl->Entries.size(); }
uint64_t FrameGraphTemplateCache::BuildCount() const noexcept { return _impl->Builds; }
void FrameGraphTemplateCache::Clear() noexcept {
    _impl->Entries.clear();
    _impl->Clock = _impl->Builds = 0;
}
vector<RenderGraphOutputBinding> FrameGraphTemplateCache::Compose(FrameGraph& frame, Nullable<RenderPipeline*> pipeline,
                                                                  std::span<RenderGraphComponent* const> overlays, Nullable<RenderSystem*> renderer) {
    RADRAY_PROFILE_SCOPE_N("FrameGraph::BindDefaultTemplate");
    if (!overlays.empty() && !renderer) {
        frame.Graph.AddDiagnostic("DefaultComposition", "Overlay composition requires a render system for blit preparation");
        return {};
    }
    auto& impl = *_impl;
    auto found = std::find_if(impl.Entries.begin(), impl.Entries.end(), [&](const auto& entry) { return Matches(entry, frame, bool(pipeline), overlays.size()); });
    if (found == impl.Entries.end()) {
        auto built = BuildComposition(frame, bool(pipeline), overlays.size());
        if (!built.Template) {
            frame.Graph.AddDiagnostic("DefaultComposition", "Default composition template could not be frozen");
            return {};
        }
        ++impl.Builds;
        if (impl.Entries.size() < impl.Capacity) {
            impl.Entries.push_back(std::move(built));
            found = std::prev(impl.Entries.end());
        } else {
            found = std::min_element(impl.Entries.begin(), impl.Entries.end(), [](const auto& a, const auto& b) { return a.LastUsed < b.LastUsed; });
            *found = std::move(built);
        }
    }
    auto& entry = *found;
    entry.LastUsed = ++impl.Clock;
    const auto instance = frame.Graph.Instantiate(entry.Template);
    if (!instance.IsValid()) return {};
    for (size_t index = 0; index < entry.Outputs.size(); ++index)
        if (!frame.Context.BindOutputTarget(frame.Graph, instance, entry.ExternalSlots[index], entry.Outputs[index].Id).IsValid()) return {};
    if (entry.Clear.IsValid() && !instance.Bind(entry.Clear, make_shared<ClearFrame>())) return {};
    for (const auto& blit : entry.Blits)
        if (!BindRenderGraphBlit(instance, blit, *renderer, frame.Context.Backend())) return {};
    vector<FrameGraphComponentPort> ports;
    ports.reserve(entry.Outputs.size());
    for (size_t index = 0; index < entry.Components.size(); ++index) {
        ports.clear();
        for (const auto& port : entry.Components[index]) ports.push_back({port.Id, instance.Value(port.Input), instance.Value(port.Output)});
        auto& component = pipeline && index == 0 ? static_cast<RenderGraphComponent&>(*pipeline) : *overlays[index - size_t(bool(pipeline))];
        if (!frame.AddComponent(component, ports).IsValid()) return {};
    }
    vector<RenderGraphOutputBinding> exported;
    exported.reserve(entry.Exports.size());
    for (const auto& output : entry.Exports) exported.push_back({output.Output, instance.Value(output.Texture)});
    return exported;
}
FrameGraphTemplateCache& RenderPipelineContext::GetDefaultCompositionCache() {
    auto& cache = DefaultCompositionCacheStorage();
    if (!cache) cache = make_shared<FrameGraphTemplateCache>();
    return *cache;
}
void ComposeDefaultFrameGraph(FrameGraph& frame, Nullable<RenderPipeline*> pipeline) {
    frame.Context.GetDefaultCompositionCache().Compose(frame, pipeline, {}, nullptr);
}
vector<RenderGraphOutputBinding> ComposeDefaultFrameGraph(FrameGraph& frame, Nullable<RenderPipeline*> pipeline,
                                                          std::span<RenderGraphComponent* const> overlays, RenderSystem& renderer) {
    return frame.Context.GetDefaultCompositionCache().Compose(frame, pipeline, overlays, &renderer);
}
}  // namespace radray
