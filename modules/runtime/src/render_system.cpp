#include <radray/runtime/render_system.h>

#include <algorithm>
#include <filesystem>
#include <optional>
#include <span>

#include <radray/file.h>
#include <radray/logger.h>
#include <radray/render/rhi.h>
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
    // PSO 存了 RenderPass 与 PipelineLayout 裸指针, 必须先于 registry 与 (透过条目里的
    // StreamingAssetRef) 资产销毁。
    _pipelineStateCache.reset();
    // 缓存的 RenderPass / Framebuffer 必须先于 GpuSystem 持有的 device 销毁。
    _renderPassRegistry.reset();
    // 【刻意允许仍有资产引用着 layout】: 本缓存只是非拥有索引, 析构时切断残留条目的
    // 反向指针, 那些 layout 由 ShaderPassProgram 的引用计数保命, 待 AssetManager 析构
    // (在本对象之后, 见 Application::Shutdown) 时自毁。
    _pipelineLayoutCache.reset();
    // context 借用 Dxc*, 必须先于它销毁。
    _shaderResolveContext.reset();
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

    _renderPassRegistry = make_unique<RenderPassRegistry>(device);
    _pipelineStateCache = make_unique<PipelineStateCache>(device);
    _pipelineLayoutCache = make_unique<PipelineLayoutCache>(device);

    // include 根无条件推导: 关掉 JIT 的发布包同样要用它做 AOT 的源码身份复核 (若源码
    // 确实部署了), 且 ShaderResolveContext 需要一个确定的根而非"猜"。
    _shaderIncludeRoot = (GetExecutableDirectory() / "shaderlib").string();
#if defined(RADRAY_ENABLE_SHADER_JIT)
    auto dxcOpt = render::CreateDxc();
    if (!dxcOpt.HasValue()) {
        RADRAY_ERR_LOG("RenderSystem::OnInitialize: CreateDxc failed");
    } else {
        _dxc = dxcOpt.Release();
    }
#endif

    // 【策略在此一次定死】: 有 DXC 即开发构建 (Strict + JIT, 改 shader 立刻生效);
    // 没有即发布包 (Lenient + 无 JIT, 缺产物是显式错误, 且不要求源码部署)。
    // 这个判断刻意不下放到每个加载点 —— 那会让同一进程内出现两套过期语义。
    const bool developerBuild = _dxc != nullptr;
    _shaderResolveContext = make_unique<ShaderResolveContext>(
        ShaderResolveSettings{
            .ShaderRoot = std::filesystem::path{_shaderIncludeRoot},
            .Staleness = developerBuild ? ShaderArtifactStaleness::Strict
                                       : ShaderArtifactStaleness::Lenient,
            .AllowJit = developerBuild},
        _dxc.get());
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
