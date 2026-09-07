#include <radray/runtime/render_system.h>

#include "shader_program_cache.h"
#include "presentation_adapter.h"

#include <algorithm>
#include <optional>
#include <span>
#include <utility>

#include <radray/logger.h>
#include <radray/render/rhi.h>
#include <radray/runtime/application.h>
#include <radray/runtime/gpu_system.h>
#include <radray/runtime/render_framework/scene.h>
#include <radray/runtime/shader_program.h>

namespace radray {

RenderSystem::RenderSystem(Application* app) noexcept
    : _app(app) {
}

RenderSystem::~RenderSystem() noexcept {
    OnShutdown();
}

void RenderSystem::OnShutdown() noexcept {
    ReleaseAllScenes();
    _graphComposer.reset();
    _pipeline.reset();
    _graphRuntime.reset();
    _viewStates.reset();
    _retainedAssets.clear();
    _shaderCache.reset();
    _presentation.reset();
    // 缓存的 RenderPass / Framebuffer 必须先于 GpuSystem 持有的 device 销毁。
    _renderPassRegistry.reset();
    _framePlans.clear();
    _frameOutputInfos.clear();
    _graphReports.clear();
}

ServiceStatus RenderSystem::OnInitialize() {
    auto gpu = _gpuSystem;
    if (_app == nullptr || !gpu || gpu->GetDevice() == nullptr) {
        return ServiceStatus::Failure("Application, GpuSystem or Device is missing");
    }
    render::Device* device = gpu->GetDevice();

    _retainedAssets.resize(gpu->GetFlightDataCount());
    _framePlans.resize(gpu->GetFlightDataCount());
    _frameOutputInfos.resize(gpu->GetFlightDataCount());
    _graphReports.resize(gpu->GetFlightDataCount());
    _renderPassRegistry = make_unique<render::RenderPassRegistry>(device);
    _graphRuntime = make_unique<RenderGraphRuntime>(*device, *_renderPassRegistry, gpu->GetFlightDataCount());
    _viewStates = make_unique<ViewStateRegistry>(*device, *_renderPassRegistry, gpu->GetFlightDataCount());
    _presentation = make_unique<PresentationAdapter>(*_app->GetWindowManager());
    _shaderCache = make_unique<ShaderProgramCache>(*device, _app->GetShaderSourceRoot(), _app->GetShaderIncludePaths());
    return {};
}

Nullable<ShaderProgram*> RenderSystem::GetOrCreateShaderProgram(const ShaderProgramRequest& request) {
    return _shaderCache ? _shaderCache->GetOrCreateShaderProgram(request) : nullptr;
}
Nullable<ShaderProgram*> RenderSystem::GetOrCreateShaderProgram(std::span<const byte> bytes, const shader::GpuArtifactHash& identity,
                                                                const render::ShaderProgramLayoutRecipe& recipe) {
    return _shaderCache ? _shaderCache->GetOrCreateShaderProgram(bytes, identity, recipe) : nullptr;
}
size_t RenderSystem::GetShaderProgramCacheSize() const noexcept { return _shaderCache ? _shaderCache->GetProgramCount() : 0; }
size_t RenderSystem::GetShaderArtifactCacheSize() const noexcept { return _shaderCache ? _shaderCache->GetArtifactCount() : 0; }
bool RenderSystem::InvalidateShaderSource(std::string_view sourceName) { return _shaderCache && _shaderCache->InvalidateSource(sourceName); }

bool RenderSystem::SetGraphComposer(unique_ptr<FrameGraphComposer> composer) noexcept {
    if (std::this_thread::get_id() != _gameThread || (_pipelineStarted.load(std::memory_order_acquire) && !_pipelineShutdownIdle)) return false;
    _graphComposer = std::move(composer);
    return true;
}

bool RenderSystem::AddOverlay(RenderGraphComponent& overlay) noexcept {
    if (std::this_thread::get_id() != _gameThread || (_pipelineStarted.load(std::memory_order_acquire) && !_pipelineShutdownIdle)) return false;
    if (std::find(_overlays.begin(), _overlays.end(), &overlay) != _overlays.end()) return false;
    _overlays.push_back(&overlay);
    return true;
}

bool RenderSystem::RemoveOverlay(RenderGraphComponent& overlay) noexcept {
    if (std::this_thread::get_id() != _gameThread || (_pipelineStarted.load(std::memory_order_acquire) && !_pipelineShutdownIdle)) return false;
    const auto it = std::find(_overlays.begin(), _overlays.end(), &overlay);
    if (it == _overlays.end()) return false;
    _overlays.erase(it);
    return true;
}

bool RenderSystem::SetPipeline(unique_ptr<RenderPipeline> pipeline) noexcept {
    if (std::this_thread::get_id() != _gameThread) {
        RADRAY_ERR_LOG("Pipeline installation must run on the game thread");
        return false;
    }
    if (_pipelineStarted.load(std::memory_order_acquire) && !_pipelineShutdownIdle) {
        if (_pipeline) {
            RADRAY_ERR_LOG("Pipeline replacement requires initialization or the host's GPU-idle shutdown phase");
            return false;
        }
        if (!pipeline) return true;
        // Loading may have submitted fallback frames. Drain them before publishing a pipeline
        // whose PrepareFrame has not run for those already-published frame plans.
        _outputs.EnsureRenderIdle();
    }
    _pipeline = std::move(pipeline);
    return true;
}

void RenderSystem::BeginUpdateForFlight(uint32_t flightIndex) {
    RADRAY_ASSERT(flightIndex < _retainedAssets.size());
    _retainedAssets[flightIndex].clear();
    _framePlans[flightIndex].Reset();
}

void RenderSystem::PrepareFrame(const AppUpdateContext& ctx) {
    _pipelineStarted.store(true, std::memory_order_release);
    RADRAY_ASSERT(ctx.FlightIndex < _retainedAssets.size());
    auto& outputs = _frameOutputInfos[ctx.FlightIndex];
    outputs = _presentation->GetOutputInfos(_outputs);
    RenderWorkloadBuilder workloads(_framePlans[ctx.FlightIndex], outputs);
    RenderPrepareContext prepare{ctx, outputs, workloads, _retainedAssets[ctx.FlightIndex]};
    if (_pipeline)
        _pipeline->PrepareFrame(prepare);
    else
        workloads.AddPresentationOutputs();
    for (auto* overlay : _overlays) overlay->PrepareFrame(prepare);
    if (_graphComposer) _graphComposer->PrepareFrame(prepare);
    for (const auto& diagnostic : _framePlans[ctx.FlightIndex].Diagnostics) RADRAY_ERR_LOG("Render workload: {}", diagnostic);
}

void RenderSystem::Render(AppFrameContext& ctx) {
    _pipelineStarted.store(true, std::memory_order_release);
    if (!_graphRuntime || !_viewStates) return;
    const uint32_t flight = ctx.FlightIndex();
    const uint64_t serial = ctx.FrameSerial();
    auto& graphResources = _graphRuntime->BeginFlight(flight, serial, ctx.GetHostWrites());
    _viewStates->BeginFlight(flight, serial);
    auto& report = _graphReports[flight];
    report = {};
    auto outputFrame = _presentation->Acquire(ctx, _outputs, _frameOutputInfos[flight], _framePlans[flight].Outputs);
    auto& surfaces = outputFrame.Surfaces;
    const auto& resolvedOutputs = outputFrame.Outputs;
    vector<ResolvedRenderViewFamily> families;
    for (uint32_t index = 0; index < _framePlans[flight].ViewFamilies.size(); ++index) {
        const auto& requested = _framePlans[flight].ViewFamilies[index];
        RenderOutputInfo info{.Id = requested.Output};
        for (const auto& resolved : resolvedOutputs)
            if (resolved.Id == requested.Output) info = resolved;
        string reason;
        auto family = ResolveRenderViewFamily(requested, info, index, ctx.GetDevice()->GetCapabilities().Limits.MaxTexture2DDimension, reason);
        if (!family) {
            RADRAY_ERR_LOG("View family '{}': {}", requested.Name, reason);
            families.push_back({.FrameLocalIndex = index, .Name = requested.Name, .OutputId = requested.Output});
            continue;
        }
        for (auto& view : family->Views) _viewStates->Resolve(view, *family);
        families.push_back(std::move(*family));
    }
    RenderPipelineContext pipelineContext(ctx, graphResources, *_renderPassRegistry, *_viewStates, serial, families, surfaces, report);
    auto graph = pipelineContext.CreateRenderGraph("Frame");
    FrameGraph frame{pipelineContext, graph};
    if (_graphComposer)
        _graphComposer->Compose(frame, _pipeline.get());
    else
        ComposeDefaultFrameGraph(frame, _pipeline.get(), _overlays, *this);
    frame.Expand();
    const auto result = pipelineContext.ExecuteGraph(graph);
    frame.Recorded(result);
    for (auto& surface : surfaces) {
        if (!surface.Written) ClearTarget(ctx, surface);
        TransitionSurface(ctx, surface, surface.RequiredFinalState);
    }
    auto submission = result.Submission;
    if (!submission) {
        submission = make_shared<FrameSubmission>(serial);
        submission->Record();
        ctx.TrackSubmission(submission);
    }
    const auto prior = submission->OnSubmitted;
    submission->OnSubmitted = [this, prior, outputFrame = std::move(outputFrame)] {
        if (prior) prior();
        for (const auto& surface : outputFrame.Surfaces) _outputs.CommitExternalState(surface);
        _presentation->Commit(outputFrame);
    };
}

void RenderSystem::ClearTarget(AppFrameContext& ctx, RenderSurfaceFrame& target) {
    const render::RenderPassColorAttachmentDescriptor attachment{target.Desc.Format, target.Desc.SampleCount, render::LoadAction::Clear, render::StoreAction::Store};
    auto pass = _renderPassRegistry->GetOrCreateRenderPass({std::span{&attachment, 1}, {}});
    if (!pass) return;
    auto* view = target.ColorAttachmentView;
    auto framebuffer = _renderPassRegistry->GetOrCreateFramebuffer({pass.Get(), std::span{&view, 1}, nullptr, target.Desc.Width, target.Desc.Height, 1});
    if (!framebuffer) return;
    TransitionSurface(ctx, target, render::TextureState::RenderTarget);
    const render::ColorClearValue clear{{.08f, .10f, .14f, 1}};
    auto encoder = ctx.GetCommandBuffer()->BeginRenderPass({pass.Get(), framebuffer.Get(), std::span{&clear, 1}, {}, "Fallback Clear"});
    if (encoder) {
        ctx.GetCommandBuffer()->EndRenderPass(encoder.Release());
        target.Written = true;
    }
}

void RenderSystem::TransitionSurface(AppFrameContext& ctx, RenderSurfaceFrame& target, render::TextureStates state) {
    if (target.CurrentState == state) return;
    const render::ResourceBarrierDescriptor barrier = render::BarrierTextureDescriptor{.Target = target.Texture, .Before = target.CurrentState, .After = state};
    ctx.GetCommandBuffer()->ResourceBarrier(std::span{&barrier, 1});
    target.CurrentState = state;
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
