#include <radray/runtime/render_system.h>

#include <algorithm>
#include <filesystem>
#include <optional>
#include <span>

#include <radray/file.h>
#include <radray/logger.h>
#include <radray/render/common.h>
#include <radray/runtime/shader_artifact_resolver.h>
#if defined(RADRAY_ENABLE_SHADER_JIT)
#include <radray/shader/dxc.h>
#endif
#include <radray/runtime/application.h>
#include <radray/runtime/gpu_system.h>
#include <radray/runtime/render_framework/scene.h>
#include <radray/runtime/window_manager.h>

namespace radray {

RenderSystem::RenderSystem(Application* app) noexcept
    : _app(app) {
}

RenderSystem::~RenderSystem() noexcept {
    ReleaseAllScenes();
    _pipeline.reset();
    _samplerCache.reset();
    _shaderResolver.reset();
    _dxc.reset();
}

void RenderSystem::OnInitialize() {
    GpuSystem* gpu = _app != nullptr ? _app->GetGpuSystem() : nullptr;
    render::Device* device = _app != nullptr ? _app->GetDevice() : nullptr;
    AssetManager* assets = _app != nullptr ? _app->GetAssetManager() : nullptr;
    if (gpu == nullptr || device == nullptr || assets == nullptr) {
        RADRAY_ERR_LOG("RenderSystem::OnInitialize: GpuSystem, Device, or AssetManager is null");
        return;
    }

#if defined(RADRAY_ENABLE_SHADER_JIT)
    _shaderIncludeRoot = (GetExecutableDirectory() / "shaderlib").string();
#endif
    // sampler 缓存: 按 descriptor 去重 + 永生持有, 使材质快照可安全持有稳定 sampler 指针。
    _samplerCache = make_unique<SamplerCache>(device);

#if defined(RADRAY_ENABLE_SHADER_JIT)
    auto dxcOpt = shader::CreateDxc();
    if (!dxcOpt.HasValue()) {
        RADRAY_ERR_LOG("RenderSystem::OnInitialize: CreateDxc failed");
    } else {
        _dxc = dxcOpt.Release();
        _shaderResolver = make_unique<ShaderArtifactResolver>(
            *assets,
            ShaderArtifactResolver::Config{
                .ShaderRoot = _shaderIncludeRoot,
                .DiskCacheDirectory = GetExecutableDirectory() / "shader_cache",
                .EnableDiskCache = true},
            CreateDxcShaderJitCompiler(_dxc));
    }
#endif
}

Nullable<IStandardMaterialFactory*> RenderSystem::GetStandardMaterialFactory() noexcept {
    if (_pipeline == nullptr) {
        return nullptr;
    }
    return _pipeline->GetStandardMaterialFactory();
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

    if (_pipeline != nullptr) {
        RenderPipelineContext pipelineCtx(_app, ctx, targets);
        RenderCameraList cameras;
        _pipeline->BeginFrame(pipelineCtx);
        _pipeline->BuildCameraList(pipelineCtx, cameras);
        _pipeline->Render(pipelineCtx, cameras);
        _pipeline->EndFrame(pipelineCtx);
    } else {
        for (RenderPipelineTarget& target : targets) {
            EnsureRenderTargetState(ctx, target);
        }
    }

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

void RenderSystem::EnsureRenderTargetState(AppFrameContext& ctx, RenderPipelineTarget& target) {
    if (target.Target.BackBuffer == nullptr || target.State == render::TextureState::RenderTarget) {
        return;
    }

    render::ResourceBarrierDescriptor toRenderTarget = render::BarrierTextureDescriptor{
        .Target = target.Target.BackBuffer,
        .Before = target.State,
        .After = render::TextureState::RenderTarget};
    ctx.GetCommandBuffer()->ResourceBarrier(std::span{&toRenderTarget, 1});
    target.State = render::TextureState::RenderTarget;
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
