#include <radray/runtime/render_system.h>

#include <algorithm>
#include <optional>
#include <span>

#include <radray/runtime/application.h>
#include <radray/runtime/gpu_system.h>
#include <radray/runtime/render_framework/scene.h>
#include <radray/runtime/window_manager.h>

namespace radray {

RenderSystem::RenderSystem(Application* app) noexcept
    : _app(app), _pipeline(make_unique<RenderPipeline>()) {
}

RenderSystem::~RenderSystem() noexcept {
    ReleaseAllScenes();
}

void RenderSystem::Render(AppFrameContext& ctx) {
    if (_app == nullptr || _app->GetWindowManager() == nullptr) {
        return;
    }

    vector<RenderPipelineTarget> targets;
    WindowManager* windowManager = _app->GetWindowManager();
    targets.reserve(windowManager->GetWindowCount());
    const size_t windowCount = windowManager->GetWindowCount();
    for (size_t i = 0; i < windowCount; ++i) {
        AppWindow* window = windowManager->GetWindow(i);
        if (window == nullptr || window->GetSwapChain() == nullptr || window->IsMinimized()) {
            continue;
        }
        std::optional<AppFrameTarget> target = ctx.AcquireWindow(window);
        if (!target.has_value()) {
            continue;
        }
        targets.emplace_back(RenderPipelineTarget{
            .Target = target.value(),
            .State = window->GetBackBufferState(target->BackBufferIndex),
            .ContentDrawn = false});
    }
    if (targets.empty()) {
        return;
    }

    RenderPipelineContext pipelineCtx(_app, ctx, targets);
    RenderCameraList cameras;
    _pipeline->BeginFrame(pipelineCtx);
    _pipeline->BuildCameraList(pipelineCtx, cameras);
    _pipeline->Render(pipelineCtx, cameras);
    _pipeline->EndFrame(pipelineCtx);

    vector<AppSubsystem*> subsystems = _app->GetSubsystems();
    for (AppSubsystem* subsystem : subsystems) {
        subsystem->OnRenderBegin(ctx);
    }

    for (RenderPipelineTarget& target : targets) {
        bool subsystemDrawn = false;
        for (AppSubsystem* subsystem : subsystems) {
            subsystemDrawn = subsystem->OnRender(ctx, target.Target, target.ContentDrawn || subsystemDrawn) || subsystemDrawn;
        }
        target.ContentDrawn = target.ContentDrawn || subsystemDrawn;
    }

    for (AppSubsystem* subsystem : subsystems) {
        subsystem->OnRenderEnd(ctx);
    }

    for (RenderPipelineTarget& target : targets) {
        EnsurePresentState(ctx, target);
    }
}

void RenderSystem::EnsurePresentState(AppFrameContext& ctx, RenderPipelineTarget& target) {
    AppWindow* window = target.Target.Window;
    if (window == nullptr || target.Target.BackBuffer == nullptr) {
        return;
    }

    if (target.State != render::TextureState::Present) {
        render::ResourceBarrierDescriptor toPresent = render::BarrierTextureDescriptor{
            .Target = target.Target.BackBuffer,
            .Before = target.State,
            .After = render::TextureState::Present};
        ctx.GetCommandBuffer()->ResourceBarrier(std::span{&toPresent, 1});
        target.State = render::TextureState::Present;
    }
    window->SetBackBufferState(target.Target.BackBufferIndex, render::TextureState::Present);
}

Scene* RenderSystem::AllocateScene() {
    auto scene = make_unique<Scene>();
    Scene* ptr = scene.get();
    _scenes.push_back(std::move(scene));
    return ptr;
}

void RenderSystem::ReleaseScene(Scene* scene) noexcept {
    if (scene == nullptr) {
        return;
    }

    auto sceneIt = std::find_if(_scenes.begin(), _scenes.end(),
                                [scene](const unique_ptr<Scene>& ptr) {
                                    return ptr.get() == scene;
                                });
    if (sceneIt != _scenes.end()) {
        _scenes.erase(sceneIt);
    }
}

void RenderSystem::ReleaseAllScenes() noexcept {
    _scenes.clear();
}

}  // namespace radray
