#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string_view>

#include <radray/nullable.h>
#include <radray/render/rhi.h>
#include <radray/runtime/gpu_system.h>
#include <radray/runtime/render_framework/render_types.h>
#include <radray/types.h>

namespace radray {

class Application;
class AppFrameContext;
class CameraComponent;
class Scene;

enum class RenderPassEvent : int32_t {
    BeforeRendering = 0,
    BeforeRenderingShadows = 50,
    AfterRenderingShadows = 100,
    BeforeRenderingPrePasses = 150,
    AfterRenderingPrePasses = 200,
    BeforeRenderingOpaques = 250,
    AfterRenderingOpaques = 300,
    BeforeRenderingSkybox = 350,
    AfterRenderingSkybox = 400,
    BeforeRenderingTransparents = 450,
    AfterRenderingTransparents = 500,
    BeforeRenderingPostProcessing = 550,
    AfterRenderingPostProcessing = 600,
    AfterRendering = 1000,
};

struct RenderPipelineTarget {
    AppFrameTarget Target;
    render::TextureStates State{render::TextureState::Undefined};
    bool ContentDrawn{false};
};

struct RenderPipelineContext {
    RenderPipelineContext(Application* app, AppFrameContext& frame, std::span<RenderPipelineTarget> targets) noexcept;

    Application* App{nullptr};
    AppFrameContext& Frame;
    std::span<RenderPipelineTarget> Targets;
};

struct RenderCamera {
    RenderCamera(Scene* scene, CameraComponent* camera, Nullable<AppFrameTarget*> target = nullptr) noexcept;

    Scene* RenderScene;
    CameraComponent* ViewCamera;
    Nullable<AppFrameTarget*> Target{nullptr};
};

class RenderCameraList {
public:
    void Add(RenderCamera camera);
    void Add(Scene* scene, CameraComponent* camera, Nullable<AppFrameTarget*> target = nullptr);
    void Clear() noexcept;

    bool Empty() const noexcept;
    std::size_t Size() const noexcept;

    std::span<RenderCamera> Cameras() noexcept;
    std::span<const RenderCamera> Cameras() const noexcept;

private:
    vector<RenderCamera> _cameras;
};

class RenderPipelinePass {
public:
    explicit RenderPipelinePass(RenderPassEvent event = RenderPassEvent::AfterRendering) noexcept;
    RenderPipelinePass(const RenderPipelinePass&) = delete;
    RenderPipelinePass(RenderPipelinePass&&) = delete;
    RenderPipelinePass& operator=(const RenderPipelinePass&) = delete;
    RenderPipelinePass& operator=(RenderPipelinePass&&) = delete;
    virtual ~RenderPipelinePass() noexcept;

    RenderPassEvent GetRenderPassEvent() const noexcept;
    void SetRenderPassEvent(RenderPassEvent event) noexcept;

    virtual void Setup(RenderPipelineContext& ctx, const RenderCamera& camera);
    virtual void Execute(RenderPipelineContext& ctx, const RenderCamera& camera);
    virtual void Cleanup(RenderPipelineContext& ctx, const RenderCamera& camera);

protected:
    void MarkContentDrawn() noexcept { _contentDrawn = true; }

private:
    friend class RenderPipeline;

    RenderPassEvent _event{RenderPassEvent::AfterRendering};
    bool _contentDrawn{false};
};

class RenderPipeline {
public:
    RenderPipeline() noexcept = default;
    RenderPipeline(const RenderPipeline&) = delete;
    RenderPipeline(RenderPipeline&&) = delete;
    RenderPipeline& operator=(const RenderPipeline&) = delete;
    RenderPipeline& operator=(RenderPipeline&&) = delete;
    virtual ~RenderPipeline() noexcept;

    /// 公共帧入口。与 Unity SRP 不同，这些入口不会由引擎传入一个已经准备好的
    /// List<Camera>；视口目标由 RenderSystem/SceneRenderer 管理，并允许管线构建或使用
    /// RenderCameraList。
    void BeginFrame(RenderPipelineContext& ctx);
    void BuildCameraList(RenderPipelineContext& ctx, RenderCameraList& cameras);
    void Render(RenderPipelineContext& ctx, const RenderCameraList& cameras);
    void EndFrame(RenderPipelineContext& ctx);

    void EnqueuePass(RenderPipelinePass* pass);
    void ClearPasses() noexcept;

    std::span<RenderPipelinePass*> ActivePasses() noexcept;
    std::span<RenderPipelinePass* const> ActivePasses() const noexcept;

protected:
    /// 具体管线的重写点。这些重写点对应 SRP 的帧/相机阶段，但不会直接封装
    /// ScriptableRenderContext.Submit；命令提交仍由 GpuSystem/RenderSystem 的帧流程负责。
    virtual void OnBeginFrame(RenderPipelineContext& ctx);
    virtual void OnBuildCameraList(RenderPipelineContext& ctx, RenderCameraList& cameras);
    virtual void OnRender(RenderPipelineContext& ctx, const RenderCameraList& cameras);
    virtual void OnEndFrame(RenderPipelineContext& ctx);

    virtual void OnRenderCamera(RenderPipelineContext& ctx, const RenderCamera& camera);
    virtual void OnSetupCamera(RenderPipelineContext& ctx, const RenderCamera& camera);
    virtual void OnSetupCulling(RenderPipelineContext& ctx, const RenderCamera& camera);
    virtual void OnSetupLights(RenderPipelineContext& ctx, const RenderCamera& camera);
    virtual void OnAddRenderPasses(RenderPipelineContext& ctx, const RenderCamera& camera);
    virtual void OnExecutePasses(RenderPipelineContext& ctx, const RenderCamera& camera);
    virtual void OnFinishCamera(RenderPipelineContext& ctx, const RenderCamera& camera);

    void PrepareTargets(RenderPipelineContext& ctx);
    void TransitionTarget(RenderPipelineContext& ctx, RenderPipelineTarget& target, render::TextureStates after);
    bool RenderTargetContent(RenderPipelineContext& ctx, RenderPipelineTarget& target);
    void ClearTarget(RenderPipelineContext& ctx, RenderPipelineTarget& target, std::string_view name);

private:
    vector<RenderPipelinePass*> _activePasses;
};

}  // namespace radray
