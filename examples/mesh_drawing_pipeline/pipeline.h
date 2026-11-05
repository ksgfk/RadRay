#pragma once

#include <radray/runtime/mesh_pass_processor.h>
#include <radray/runtime/render_graph_pipeline.h>
#include <radray/runtime/scene.h>

#include "gpu_support.h"

namespace radray::examples::mesh_drawing {

// 本例三份 Material 使用相同参数声明顺序；索引只在准备阶段按名称解析。
struct MaterialParameterIndices {
    size_t BaseColor;
    size_t Brightness;
};

enum class PassParticipation {
    DepthAndColor,
    DepthOnly,
    ColorOnly
};

struct BatchPassPolicy {
    SparseSetHandle Batch;
    PassParticipation Participation;
    std::string_view ObjectName;
};

struct PassContext {
    Eigen::Matrix4f ViewProjection;
    Viewport ViewportRect;
    Rect Scissor;
};

class DepthPassProcessor final : public MeshPassProcessor {
public:
    DepthPassProcessor(MeshPassDrawListContext& drawListContext, const Scene& scene, std::span<const MaterialPipelines> pipelines,
                       std::span<const BatchPassPolicy> policies, MaterialTagFilter materialFilter) noexcept;
    void AddMeshBatch(const MeshBatchContext& context) override;
    bool HasErrors{false};

private:
    const Scene* _scene;
    std::span<const MaterialPipelines> _pipelines;
    std::span<const BatchPassPolicy> _policies;
    MaterialTagFilter _materialFilter;
};

class ColorPassProcessor final : public MeshPassProcessor {
public:
    ColorPassProcessor(MeshPassDrawListContext& drawListContext, const Scene& scene, std::span<const MaterialPipelines> pipelines,
                       std::span<const BatchPassPolicy> policies, MaterialTagFilter materialFilter, MaterialParameterIndices parameters) noexcept;
    void AddMeshBatch(const MeshBatchContext& context) override;
    bool HasErrors{false};

private:
    const Scene* _scene;
    std::span<const MaterialPipelines> _pipelines;
    std::span<const BatchPassPolicy> _policies;
    MaterialTagFilter _materialFilter;
    MaterialParameterIndices _parameters;
};

class OutlinePassProcessor final : public MeshPassProcessor {
public:
    OutlinePassProcessor(MeshPassDrawListContext& drawListContext, const Scene& scene, std::span<const MaterialPipelines> pipelines,
                         std::span<const BatchPassPolicy> policies, MaterialTagFilter opaqueFilter, MaterialTagFilter outlineFilter) noexcept;
    void AddMeshBatch(const MeshBatchContext& context) override;
    bool HasErrors{false};

private:
    const Scene* _scene;
    std::span<const MaterialPipelines> _pipelines;
    std::span<const BatchPassPolicy> _policies;
    MaterialTagFilter _opaqueFilter;
    MaterialTagFilter _outlineFilter;
};

class TransparentPassProcessor final : public MeshPassProcessor {
public:
    TransparentPassProcessor(MeshPassDrawListContext& drawListContext, const Scene& scene, std::span<const MaterialPipelines> pipelines,
                             std::span<const BatchPassPolicy> policies, MaterialTagFilter materialFilter, MaterialParameterIndices parameters) noexcept;
    void AddMeshBatch(const MeshBatchContext& context) override;
    bool HasErrors{false};

private:
    const Scene* _scene;
    std::span<const MaterialPipelines> _pipelines;
    std::span<const BatchPassPolicy> _policies;
    MaterialTagFilter _materialFilter;
    MaterialParameterIndices _parameters;
};

class MeshDrawingFramePipeline final : public RenderGraphFramePipeline {
public:
    // Shared resources, source scene, candidates and policies must outlive the scheduler.
    MeshDrawingFramePipeline(GpuResources& gpu, const Scene& scene, const PassContext& view,
                             std::span<const SparseSetHandle> candidates, std::span<const BatchPassPolicy> policies,
                             MaterialParameterIndices parameters, uint32_t slotCount) noexcept;
    // Call once before transferring ownership to FrameScheduler.
    bool Initialize();
    std::span<const MeshPassRegistration> GetMeshPassRegistrations() noexcept override;
    void Wait(FrameSlot& slot) noexcept override;
    bool HasErrors() const noexcept;
    // Call after scheduler.WaitIdle(); the pipeline remains owned by the scheduler.
    bool SaveImage(uint32_t slotIndex, const std::filesystem::path& output) const;

private:
    void ExtractFrameData(const Scene& source, FrameSlot& slot) noexcept override;
    void SetupGraph(RDGBuilder& graph, FrameSlot& slot) noexcept override;
    void ExtractMeshPass(MeshPassId pass, const Scene& source, FrameSlot& slot, MeshPassFrameDrawList& drawList) noexcept override;
    void SubmitFrame(FrameSlot& slot, render::CommandBuffer& commandBuffer) noexcept override;
    bool RecordPass(render::GraphicsCommandEncoder& encoder, FrameSlot& slot, MeshPassId passId, const GpuFrameResources& resources);

    struct ExtractedFramePayload;

    GpuResources& _gpu;
    const Scene& _scene;
    PassContext _view;
    std::span<const SparseSetHandle> _candidates;
    std::span<const BatchPassPolicy> _policies;
    DynamicMeshPassDrawListContext _defaultOutput;
    DepthPassProcessor _depthProcessor;
    ColorPassProcessor _colorProcessor;
    OutlinePassProcessor _outlineProcessor;
    TransparentPassProcessor _transparentProcessor;
    array<MeshPassRegistration, 4> _passes;
    vector<unique_ptr<GpuFrameResources>> _frames;
    vector<Nullable<ExtractedFramePayload*>> _payloads;
    bool _geometrySubmitted{false};
};

bool PreparePassDrawCommands(MeshPassId passId, const Scene& scene,
                             std::span<const SparseSetHandle> primitives, std::span<const BatchPassPolicy> policies,
                             MeshPassFrameDrawList& output);

}  // namespace radray::examples::mesh_drawing
