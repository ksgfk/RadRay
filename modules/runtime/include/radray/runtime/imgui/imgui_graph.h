#pragma once

#ifdef RADRAY_ENABLE_IMGUI

#include <radray/runtime/imgui/imgui_system.h>
#include <radray/runtime/render_framework/render_pipeline.h>

namespace radray {

struct ImGuiGraphImageBinding {
    ImTextureID Image{0};
    RgTextureViewHandle View{};
};
struct ImGuiSceneOutput {
    RenderOutputId Output;
    RgTextureHandle Texture;
    ImGuiColorEncoding SampleEncoding{ImGuiColorEncoding::Srgb};
};
class ImGuiGraph {
public:
    /// Called explicitly before scene BuildGraph. Only camera-backed outputs are redirected.
    static vector<ImGuiSceneOutput> PrepareSceneOutputs(RenderGraph& graph, RenderPipelineContext& context);
    /// Adds upload, linear UI composition and final output passes to the caller's graph.
    static bool BuildGraph(RenderGraph& graph, RenderPipelineContext& context, ImGuiSystem& system,
                           std::span<const ImGuiSceneOutput> scenes = {},
                           std::span<const ImGuiGraphImageBinding> images = {});
    /// Call after the one ExecuteGraph. Acknowledgements wait for the real flight completion.
    static void CompleteGraph(const RenderGraph& graph, RenderPipelineContext& context, ImGuiSystem& system,
                              bool success);
};

class ImGuiOnlyPipeline final : public RenderPipeline {
public:
    explicit ImGuiOnlyPipeline(ImGuiSystem& system) : _system(system) {}
    void PrepareFrame(RenderPrepareContext& context) override;
    void Render(RenderPipelineContext& context) override;

private:
    ImGuiSystem& _system;
};

}  // namespace radray

#endif  // RADRAY_ENABLE_IMGUI
