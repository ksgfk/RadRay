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
    void PruneScenes(const SceneManager& scenes);
    struct PrimitiveRecord {
        const GpuMesh::DrawData* Draw;
        const StaticMeshSection* Section;
        MaterialBinding Binding;
    };
    /// Derived persistent records. Borrow expires at Apply; refresh checks shape generation and
    /// geometry/material revisions before touching old asset addresses.
    std::span<const PrimitiveRecord> GetObjectRecords(SceneId sceneId, const RenderScene& scene, ShapeId id);
    std::optional<PreparedSceneDraw> Prepare(
        PipelineContext& context, const SceneViewRequest& view, const SceneGpuView& objects,
        render::RenderPass* pass, const RenderOutputInfo& output, std::string_view passName,
        Nullable<const MaterialRenderData*> fallback, bool depthTest, Nullable<MaterialGpuState*> fallbackGpu = nullptr);

private:
    RasterDraw _draw;
    struct ObjectRecord {
        ShapeId Id{};
        uint64_t Geometry{0}, Materials{0};
        shared_ptr<const vector<PrimitiveRecord>> Items;
    };
    struct TemplateKey {
        const StaticMeshRenderData* Mesh;
        const MaterialBinding* Materials;
        size_t Count;
        friend bool operator==(const TemplateKey&, const TemplateKey&) = default;
    };
    struct TemplateHash {
        size_t operator()(const TemplateKey& key) const noexcept;
    };
    struct SceneCache {
        uint64_t Revision{UINT64_MAX};
        vector<ObjectRecord> Records;
        vector<uint32_t> Active;
        unordered_map<TemplateKey, shared_ptr<const vector<PrimitiveRecord>>, TemplateHash> Templates;
    };
    unordered_map<uint64_t, SceneCache> _scenes;
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
