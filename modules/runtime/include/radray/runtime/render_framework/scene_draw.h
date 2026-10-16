#pragma once
#include <radray/runtime/render_framework/material.h>
#include <radray/runtime/render_framework/render_pipeline.h>
#include <radray/runtime/render_framework/render_scene.h>
#include <radray/runtime/render_framework/scene_gpu.h>
#include <radray/runtime/render_framework/renderer.h>

namespace radray {
/// Non-owning compiled draw IDs; frame storage and material/resource pins belong to Renderer.
struct PreparedSceneDraw {
    SceneViewData View;
    vector<MaterialId> Materials;
    vector<FrameDrawId> Draws;
    /// Actual preparation diagnostics, suitable for producers deciding whether an explicit pass is supported.
    struct Diagnostics {
        size_t MaskCulled{0}, BoundsCulled{0}, MissingMaterial{0}, MissingPass{0};
        size_t DirectObjects{0}, AutomaticDisabled{0}, OrderedObjects{0}, ExplicitUnsupported{0};
    } Stats;
    bool Record(FrameBuilder& frame, render::GraphicsCommandEncoder* encoder) const;
};
class SceneDraw {
public:
    std::optional<PreparedSceneDraw> Prepare(
        PipelineContext& context, const SceneViewRequest& view, const SceneGpuView& objects,
        const RenderOutputInfo& output, std::string_view passName,
        std::optional<MaterialId> fallback, bool depthTest);
};

struct ScenePipelineDescriptor {
    string PassName{"Unlit"};
    /// Registered fallback for callers without material bindings; absent means diagnose/skip.
    std::optional<MaterialId> Fallback;
    bool DepthTest{false};
    render::ColorClearValue ClearColor{{0.025f, 0.035f, 0.055f, 1}};
};
/// Reference raster pipeline. Select "DepthOnly" with a depth output for a second material contract.
class UnlitRenderPipeline final : public RenderPipeline {
public:
    explicit UnlitRenderPipeline(ScenePipelineDescriptor descriptor = {});
    unique_ptr<Plan> CreatePlan(const RenderPipelineRequest& request) override;

private:
    ScenePipelineDescriptor _descriptor;
};
}  // namespace radray
