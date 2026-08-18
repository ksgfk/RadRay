#pragma once

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
