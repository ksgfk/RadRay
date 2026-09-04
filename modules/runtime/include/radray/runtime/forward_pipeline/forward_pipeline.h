#pragma once

#include <radray/render/shader_layout.h>
#include <radray/runtime/render_framework/render_pipeline.h>
#include <radray/types.h>

namespace radray {

namespace forward_detail {
struct ForwardFrameInput;
struct ForwardPipelineTestAccess;
}  // namespace forward_detail

class Application;
class CameraComponent;
class Scene;

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

private:
    friend struct forward_detail::ForwardPipelineTestAccess;
    const forward_detail::ForwardFrameInput& GetFrameInput(uint32_t flightIndex) const noexcept;

    struct Impl;

    unique_ptr<Impl> _impl;
};

}  // namespace radray
