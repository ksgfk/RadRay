#pragma once
#include <radray/runtime/render_framework/material.h>
#include <radray/runtime/render_framework/render_pipeline.h>
#include <radray/runtime/render_framework/render_scene.h>
#include <radray/runtime/render_framework/scene_gpu.h>
#include <radray/runtime/render_framework/raster_draw.h>

namespace radray {
using PreparedSceneMaterial = PreparedRasterMaterial;
struct SceneDrawKey {
    const GpuMesh::DrawData* Geometry;
    const MaterialRenderData* Material;
    friend bool operator==(const SceneDrawKey&, const SceneDrawKey&) = default;
};
struct SceneDrawKeyHash {
    size_t operator()(const SceneDrawKey& key) const noexcept;
};
/// Transient preparation borrows the scene until its next Apply; no asset ownership on RT.
struct PreparedSceneDraw {
    struct Item {
        PreparedRasterDraw Draw;
        size_t Material;
    };
    SceneViewData View;
    vector<PreparedSceneMaterial> Materials;
    vector<Item> Draws;
    /// Actual preparation diagnostics, suitable for producers deciding whether an explicit pass is supported.
    struct Diagnostics {
        size_t MaskCulled{0}, BoundsCulled{0}, MissingMaterial{0}, MissingPass{0};
        size_t DirectObjects{0}, AutomaticDisabled{0}, OrderedObjects{0}, ExplicitUnsupported{0};
    } Stats;
    void Record(render::GraphicsCommandEncoder* encoder) const;
};
class SceneDraw {
public:
    std::optional<PreparedSceneDraw> Prepare(
        PipelineContext& context, const SceneViewRequest& view, const SceneGpuView& objects,
        const RenderOutputInfo& output, std::string_view passName,
        Nullable<const MaterialRenderData*> fallback, bool depthTest, Nullable<MaterialGpuState*> fallbackGpu = nullptr);

private:
    RasterDraw _draw;
};

struct ScenePipelineDescriptor {
    string PassName{"Unlit"};
    /// Explicit compatibility material for callers without bindings; absent means diagnose/skip.
    std::optional<MaterialRenderData> Fallback;
    bool DepthTest{false};
    render::ColorClearValue ClearColor{{0.025f, 0.035f, 0.055f, 1}};
};
/// Reference raster pipeline. Select "DepthOnly" with a depth output for a second material contract.
class UnlitRenderPipeline final : public RenderPipeline {
public:
    explicit UnlitRenderPipeline(ScenePipelineDescriptor descriptor = {});
    PipelineRecordResult Record(PipelineContext& context, const RenderPipelineRequest& request) override;

private:
    ScenePipelineDescriptor _descriptor;
    SceneDraw _draw;
    MaterialGpuState _fallbackGpu;
};
}  // namespace radray
