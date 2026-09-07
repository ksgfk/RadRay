#pragma once

#include <radray/imgui/imgui_system.h>
#include <radray/runtime/render_framework/frame_graph.h>

namespace radray {

struct UiFlight;
struct UiGraphResources;
/// Borrowed state for one flight. The system retains all referenced assets until that flight completes.
class ImGuiGraphFrame {
private:
    friend class ImGuiSystem;
    friend class ImGuiGraph;
    friend class ImGuiGraphComponent;
    ImGuiGraphFrame(UiFlight& flight, UiGraphResources& resources, uint32_t index) noexcept
        : _flight(flight), _resources(resources), _index(index) {}
    UiFlight& _flight;
    UiGraphResources& _resources;
    uint32_t _index;
};

struct ImGuiGraphImageBinding {
    ImTextureID Image{0};
    RgTextureValue Texture{};
    RgTextureViewDesc View{};
};
struct ImGuiSceneOutput {
    RenderOutputId Output;
    RgTextureValue Texture;
    ImGuiColorEncoding SampleEncoding{ImGuiColorEncoding::Srgb};
};
class ImGuiGraph {
public:
    /// Declares ordinary upload/raster operations against explicit target versions.
    /// Targets may be offscreen graph textures consumed by any later graph component.
    static bool BuildGraph(RenderGraph& graph, RenderPipelineContext& context, ImGuiGraphFrame frame,
                           std::span<RenderGraphOutputBinding> targets,
                           std::span<const ImGuiSceneOutput> scenes = {},
                           std::span<const ImGuiGraphImageBinding> images = {});
    /// Call after the one ExecuteGraph. Acknowledgements wait for the real flight completion.
    static void CompleteGraph(const RenderGraph& graph, RenderPipelineContext& context, ImGuiGraphFrame frame,
                              bool success);
};

/// Ordinary RenderGraphComponent. PrepareFrame requests the captured viewport outputs. BuildGraph
/// draws the UI over the supplied target versions; for outputs registered with RegisterOutput and
/// not covered by OutputImages it copies the incoming (pre-UI) target version as the preview image,
/// inferring its encoding from the target format.
class ImGuiGraphComponent final : public RenderGraphComponent {
public:
    explicit ImGuiGraphComponent(ImGuiSystem& system) : _system(system) {}
    void PrepareFrame(RenderPrepareContext& context) override;
    void BuildGraph(RenderPipelineContext& context, RenderGraph& graph, std::span<RenderGraphOutputBinding> outputs) override;
    void GraphRecorded(RenderPipelineContext& context, const RenderGraph& graph, RenderGraphExecutionResult result) override;
    /// Custom composers may supply explicit preview images / graph image bindings before expansion.
    vector<ImGuiSceneOutput> OutputImages;
    vector<ImGuiGraphImageBinding> Images;

private:
    ImGuiSystem& _system;
};

}  // namespace radray
