#pragma once

#include <radray/render/shader_layout.h>
#include <radray/runtime/render_framework/render_pipeline.h>
#include <radray/types.h>

namespace radray {

class Application;
class CameraComponent;
class ForwardDrawPass;
class Scene;

class ForwardPipeline final : public RenderPipeline {
public:
    ForwardPipeline(
        Application* app,
        Scene* scene,
        CameraComponent* camera);
    ~ForwardPipeline() noexcept override;

    static constexpr BindingGroupPlan GetBindingGroupPlan() noexcept {
        return BindingGroupPlan{0, 1, 2};
    }

    // The pipeline uploads its view, material, and object constant buffers out of a per-frame
    // arena, so each of those declarations has to take its offset at bind time: a root descriptor
    // on D3D12 and a dynamic uniform buffer descriptor on Vulkan.
    static render::ShaderProgramLayoutRecipe GetLayoutRecipe() noexcept;

protected:
    void OnBeginFrame(RenderPipelineContext& ctx) override;
    void OnBuildCameraList(
        RenderPipelineContext& ctx,
        RenderCameraList& cameras) override;
    void OnAddRenderPasses(
        RenderPipelineContext& ctx,
        const RenderCamera& camera) override;

private:
    friend class ForwardDrawPass;

    struct Impl;

    bool ExecutePreparedPass(
        RenderPipelineContext& ctx,
        const RenderCamera& camera,
        bool transparent);

    unique_ptr<Impl> _impl;
};

}  // namespace radray
