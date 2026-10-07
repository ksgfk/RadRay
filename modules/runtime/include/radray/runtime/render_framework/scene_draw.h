#pragma once
#include <radray/runtime/render_framework/material.h>
#include <radray/runtime/render_framework/render_pipeline.h>
#include <radray/runtime/render_framework/render_scene.h>
#include <radray/runtime/render_framework/scene_gpu.h>

namespace radray {
struct PreparedSceneMaterial {
    struct Group {
        uint32_t Index;
        render::ShaderParameterSet* Parameters;
        vector<render::ShaderParameterDynamicOffset> Offsets;
    };
    vector<Group> Groups;
    render::BindingHandle ObjectIndex;
};
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
    struct Geometry {
        array<render::GraphicsPipelineState*, 2> Pipelines;
        size_t Material;
    };
    const RenderScene* Scene;
    SceneViewData View;
    Nullable<const MaterialRenderData*> Fallback;
    vector<PreparedSceneMaterial> Materials;
    unordered_map<SceneDrawKey, Geometry, SceneDrawKeyHash> GeometryBindings;
    void Record(render::GraphicsCommandEncoder* encoder) const;
};
class SceneDraw {
public:
    std::optional<PreparedSceneDraw> Prepare(PipelineContext& context, const SceneViewRequest& view, const SceneGpuView& objects,
                                             render::RenderPass* pass, const RenderOutputInfo& output, std::string_view passName,
                                             Nullable<const MaterialRenderData*> fallback, bool depthTest);

private:
    struct InputPlan {
        ShaderProgram* Program;
        vector<MaterialInput> Inputs;
        string PushName;
        vector<render::BindingHandle> Handles;
        vector<render::ShaderBindingInfo> Bindings;
        render::BindingHandle Push;
    };
    Nullable<const InputPlan*> ResolveInputs(ShaderProgram* program, const MaterialPass& pass);
    vector<unique_ptr<InputPlan>> _plans;
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
};
}  // namespace radray
