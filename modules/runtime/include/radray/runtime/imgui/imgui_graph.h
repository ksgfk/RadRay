#pragma once

#ifdef RADRAY_ENABLE_IMGUI

#include <radray/runtime/imgui/imgui_system.h>
#include <radray/runtime/render_framework/frame_graph.h>

namespace radray {

class RenderSystem;
struct UiFlight;
struct UiGraphResources;
/// Borrowed state for one flight. The system retains all referenced assets until that flight completes.
class ImGuiGraphFrame {
private:
    friend class ImGuiSystem;
    friend class ImGuiGraph;
    friend class ImGuiFrameComposer;
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

class ImGuiGraphComponent final : public RenderGraphComponent {
public:
    explicit ImGuiGraphComponent(ImGuiSystem& system) : _system(system) {}
    void BuildGraph(RenderPipelineContext& context, RenderGraph& graph, std::span<RenderGraphOutputBinding> outputs) override;
    void GraphRecorded(RenderPipelineContext& context, const RenderGraph& graph, RenderGraphExecutionResult result) override;
    vector<ImGuiSceneOutput> OutputImages;
    vector<ImGuiGraphImageBinding> Images;

private:
    ImGuiSystem& _system;
};

/// The application's default composition policy. Custom applications can install another
/// FrameGraphComposer and connect this same UI component anywhere in the graph.
class ImGuiFrameComposer final : public FrameGraphComposer {
public:
    ImGuiFrameComposer(ImGuiSystem& system, RenderSystem& renderer) : _system(system), _renderer(renderer), _component(system) {}
    void PrepareFrame(RenderPrepareContext& context) override;
    void Compose(FrameGraph& frame, Nullable<RenderPipeline*> pipeline) override;

private:
    ImGuiSystem& _system;
    RenderSystem& _renderer;
    ImGuiGraphComponent _component;
};

}  // namespace radray

#endif  // RADRAY_ENABLE_IMGUI
