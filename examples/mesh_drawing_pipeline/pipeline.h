#pragma once

#include <optional>

#include <radray/runtime/mesh_pass_processor.h>
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

// 与 cube.hlsl 的 PushData 一致；参数存储和绑定仅属于样例。
struct DrawConstants {
    array<float, 16> LocalToClip{};
    array<float, 4> Color{};
};

static_assert(sizeof(DrawConstants) == 80);

struct PreparedDraw {
    MeshDrawCommand Command;
    DrawConstants Constants;
    uint8_t StencilReference{0};
};

struct PreparedPass {
    Viewport ViewportRect;
    Rect Scissor;
    vector<PreparedDraw> Commands;
};

// Context 借用 Scene 读取材质快照，拥有本次输出以及 cube.hlsl 所需的参数副本。
class SampleDrawListContext final : public MeshPassDrawListContext {
public:
    SampleDrawListContext(const PassContext& pass, std::string_view passName,
                          const Scene& scene, MaterialParameterIndices parameters, std::span<const BatchPassPolicy> policies,
                          float localScale = 1.0f, std::optional<array<float, 4>> colorOverride = std::nullopt);
    MeshDrawCommand& AddCommand(const MeshDrawCommand& initializer) override;
    void FinalizeCommand(const MeshBatchContext& context, size_t elementIndex, MeshDrawCommand& command) override;

    PreparedPass Pass;
    bool HasErrors{false};

private:
    Eigen::Matrix4f _viewProjection;
    std::string_view _passName;
    std::span<const BatchPassPolicy> _policies;
    const Scene* _scene;
    MaterialParameterIndices _parameters;
    float _localScale;
    std::optional<array<float, 4>> _colorOverride;
};

class DepthPassProcessor final : public MeshPassProcessor {
public:
    DepthPassProcessor(SampleDrawListContext& drawListContext, const Scene& scene, std::span<const MaterialPipelines> pipelines,
                       std::span<const BatchPassPolicy> policies, MaterialTagFilter materialFilter) noexcept;
    void AddMeshBatch(const MeshBatchContext& context) override;

private:
    SampleDrawListContext* _output;
    const Scene* _scene;
    std::span<const MaterialPipelines> _pipelines;
    std::span<const BatchPassPolicy> _policies;
    MaterialTagFilter _materialFilter;
};

class ColorPassProcessor final : public MeshPassProcessor {
public:
    ColorPassProcessor(SampleDrawListContext& drawListContext, const Scene& scene, std::span<const MaterialPipelines> pipelines,
                       std::span<const BatchPassPolicy> policies, MaterialTagFilter materialFilter) noexcept;
    void AddMeshBatch(const MeshBatchContext& context) override;

private:
    SampleDrawListContext* _output;
    const Scene* _scene;
    std::span<const MaterialPipelines> _pipelines;
    std::span<const BatchPassPolicy> _policies;
    MaterialTagFilter _materialFilter;
};

class OutlinePassProcessor final : public MeshPassProcessor {
public:
    OutlinePassProcessor(SampleDrawListContext& drawListContext, const Scene& scene, std::span<const MaterialPipelines> pipelines,
                         std::span<const BatchPassPolicy> policies, MaterialTagFilter opaqueFilter, MaterialTagFilter outlineFilter) noexcept;
    void AddMeshBatch(const MeshBatchContext& context) override;

private:
    SampleDrawListContext* _output;
    const Scene* _scene;
    std::span<const MaterialPipelines> _pipelines;
    std::span<const BatchPassPolicy> _policies;
    MaterialTagFilter _opaqueFilter;
    MaterialTagFilter _outlineFilter;
};

class TransparentPassProcessor final : public MeshPassProcessor {
public:
    TransparentPassProcessor(SampleDrawListContext& drawListContext, const Scene& scene, std::span<const MaterialPipelines> pipelines,
                             std::span<const BatchPassPolicy> policies, MaterialTagFilter materialFilter) noexcept;
    void AddMeshBatch(const MeshBatchContext& context) override;

private:
    SampleDrawListContext* _output;
    const Scene* _scene;
    std::span<const MaterialPipelines> _pipelines;
    std::span<const BatchPassPolicy> _policies;
    MaterialTagFilter _materialFilter;
};

// 调用方负责 Begin/EndRenderPass。失败后必须放弃提交当前 CommandBuffer。
bool RecordDrawCommands(render::GraphicsCommandEncoder& encoder, const PreparedPass& pass);

}  // namespace radray::examples::mesh_drawing
