#pragma once

#include <radray/render/shader_layout.h>
#include <radray/runtime/render_framework/render_pipeline.h>
#include <radray/runtime/render_framework/render_scene_snapshot.h>
#include <radray/runtime/render_framework/mesh_draw_command.h>
#include <radray/types.h>

namespace radray {

namespace forward_detail {
struct ForwardViewDrawWork;
struct ForwardPipelineTestAccess;
}  // namespace forward_detail

class Application;
class CameraComponent;
class Scene;

struct ForwardStageBStats {
    uint64_t SnapshotBuilds{0}, CullCalls{0}, CullFailures{0};
    uint64_t DepthCommands{0}, OpaqueCommands{0}, TransparentCommands{0};
    DrawExecutionStats Execution;
};

class ForwardPipeline final : public RenderPipeline {
public:
    ForwardPipeline(
        Application* app,
        Scene* scene,
        CameraComponent* camera);
    ~ForwardPipeline() noexcept override;

    void PrepareFrame(RenderPrepareContext& ctx) override;
    void Render(RenderPipelineContext& ctx) override;

    // The pipeline uploads its view, material, and object constant buffers out of a per-frame
    // arena, so each of those declarations has to take its offset at bind time: a root descriptor
    // on D3D12 and a dynamic uniform buffer descriptor on Vulkan.
    static render::ShaderProgramLayoutRecipe GetLayoutRecipe() noexcept;
    static render::ShaderProgramLayoutRecipe GetDepthOnlyLayoutRecipe() noexcept;

    // Read only at the flight's phase boundary, while its owner is not updating/rendering it.
    const RenderSceneSnapshot& GetSceneSnapshot(uint32_t flightIndex) const noexcept;
    const ForwardStageBStats& GetStageBStats(uint32_t flightIndex) const noexcept;

private:
    friend struct forward_detail::ForwardPipelineTestAccess;
    std::span<const forward_detail::ForwardViewDrawWork> GetViewWork(uint32_t flightIndex, uint32_t familyIndex) const noexcept;

    struct Impl;

    unique_ptr<Impl> _impl;
};

}  // namespace radray
