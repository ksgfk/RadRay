#pragma once

#include <radray/runtime_type.h>
#include <radray/runtime/render_framework/render_pipeline.h>
#include <radray/runtime/render_framework/scene.h>
#include <radray/types.h>

namespace radray {

class Application;
class AppFrameContext;
struct AppFrameTarget;

/// Runtime-side renderer coordinator.
///
/// This fills the role that UE's RendererModule plays for scene ownership in a
/// single-module runtime: it creates, tracks, and releases render scenes.
class RenderSystem {
public:
    explicit RenderSystem(Application* app) noexcept;
    RenderSystem(const RenderSystem&) = delete;
    RenderSystem(RenderSystem&&) = delete;
    RenderSystem& operator=(const RenderSystem&) = delete;
    RenderSystem& operator=(RenderSystem&&) = delete;
    ~RenderSystem() noexcept;

    void Render(AppFrameContext& ctx);

    Scene* AllocateScene();
    void ReleaseScene(Scene* scene) noexcept;
    void ReleaseAllScenes() noexcept;

private:
    void EnsurePresentState(AppFrameContext& ctx, RenderPipelineTarget& target);

    Application* _app{nullptr};
    unique_ptr<RenderPipeline> _pipeline;
    vector<unique_ptr<Scene>> _scenes;
};

template <>
struct RuntimeTypeTrait<RenderSystem> {
    static constexpr RuntimeTypeId value{0x241d4e78, 0x8f4e, 0x4d1c, 0xa8, 0xb9, 0x55, 0x09, 0x61, 0x6a, 0x90, 0x24};
};

}  // namespace radray
