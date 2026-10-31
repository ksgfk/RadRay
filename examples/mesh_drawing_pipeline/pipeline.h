#pragma once

#include <radray/runtime/mesh_pass_processor.h>

namespace radray::examples::mesh_drawing {

struct MaterialColor {
    SparseSetHandle Material;
    array<float, 4> Color;
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
};

struct PreparedPass {
    Viewport ViewportRect;
    Rect Scissor;
    vector<PreparedDraw> Commands;
};

// Context 拥有本次输出；colors 为空时使用白色参数（Depth Pass）。
class SampleDrawListContext final : public MeshPassDrawListContext {
public:
    SampleDrawListContext(const PassContext& pass, std::string_view passName,
                          std::span<const BatchPassPolicy> policies, std::span<const MaterialColor> colors = {});
    MeshDrawCommand& AddCommand(const MeshDrawCommand& initializer) override;
    void FinalizeCommand(const MeshBatchContext& context, size_t elementIndex, MeshDrawCommand& command) override;

    PreparedPass Pass;
    bool HasErrors{false};

private:
    Eigen::Matrix4f _viewProjection;
    std::string_view _passName;
    std::span<const BatchPassPolicy> _policies;
    std::span<const MaterialColor> _colors;
};

class DepthPassProcessor final : public MeshPassProcessor {
public:
    DepthPassProcessor(MeshPassDrawListContext& drawListContext, render::GraphicsPipelineState& pipeline,
                       std::span<const BatchPassPolicy> policies) noexcept;
    void AddMeshBatch(const MeshBatchContext& context) override;

private:
    render::GraphicsPipelineState* _pipeline;
    std::span<const BatchPassPolicy> _policies;
};

class ColorPassProcessor final : public MeshPassProcessor {
public:
    ColorPassProcessor(MeshPassDrawListContext& drawListContext, render::GraphicsPipelineState& pipeline,
                       std::span<const BatchPassPolicy> policies) noexcept;
    void AddMeshBatch(const MeshBatchContext& context) override;

private:
    render::GraphicsPipelineState* _pipeline;
    std::span<const BatchPassPolicy> _policies;
};

// 调用方负责 Begin/EndRenderPass。失败后必须放弃提交当前 CommandBuffer。
bool RecordDrawCommands(render::GraphicsCommandEncoder& encoder, const PreparedPass& pass);

}  // namespace radray::examples::mesh_drawing
