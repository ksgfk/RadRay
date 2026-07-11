#include <radray/runtime/render_framework/render_pipeline.h>

#include <algorithm>

#include <radray/runtime/application.h>
#include <radray/runtime/gpu_system.h>
#include <radray/runtime/window_manager.h>

namespace radray {

RenderPipelineContext::RenderPipelineContext(Application* app, AppFrameContext& frame, std::span<RenderPipelineTarget> targets) noexcept
    : App(app), Frame(frame), Targets(targets) {}

RenderCamera::RenderCamera(Scene* scene, CameraComponent* camera, Nullable<AppFrameTarget*> target) noexcept
    : RenderScene(scene), ViewCamera(camera), Target(target) {}

void RenderCameraList::Add(RenderCamera camera) {
    _cameras.push_back(camera);
}

void RenderCameraList::Add(Scene* scene, CameraComponent* camera, Nullable<AppFrameTarget*> target) {
    _cameras.emplace_back(scene, camera, target);
}

void RenderCameraList::Clear() noexcept {
    _cameras.clear();
}

bool RenderCameraList::Empty() const noexcept {
    return _cameras.empty();
}

std::size_t RenderCameraList::Size() const noexcept {
    return _cameras.size();
}

std::span<RenderCamera> RenderCameraList::Cameras() noexcept {
    return _cameras;
}

std::span<const RenderCamera> RenderCameraList::Cameras() const noexcept {
    return _cameras;
}

RenderPipelinePass::RenderPipelinePass(RenderPassEvent event) noexcept : _event(event) {}

RenderPipelinePass::~RenderPipelinePass() noexcept = default;

RenderPassEvent RenderPipelinePass::GetRenderPassEvent() const noexcept {
    return _event;
}

void RenderPipelinePass::SetRenderPassEvent(RenderPassEvent event) noexcept {
    _event = event;
}

void RenderPipelinePass::Setup(RenderPipelineContext& ctx, const RenderCamera& camera) {
    (void)ctx;
    (void)camera;
}

void RenderPipelinePass::Execute(RenderPipelineContext& ctx, const RenderCamera& camera) {
    (void)ctx;
    (void)camera;
}

void RenderPipelinePass::Cleanup(RenderPipelineContext& ctx, const RenderCamera& camera) {
    (void)ctx;
    (void)camera;
}

RenderPipeline::~RenderPipeline() noexcept = default;

void RenderPipeline::BeginFrame(RenderPipelineContext& ctx) {
    OnBeginFrame(ctx);
}

void RenderPipeline::BuildCameraList(RenderPipelineContext& ctx, RenderCameraList& cameras) {
    OnBuildCameraList(ctx, cameras);
}

void RenderPipeline::Render(RenderPipelineContext& ctx, const RenderCameraList& cameras) {
    OnRender(ctx, cameras);
}

void RenderPipeline::EndFrame(RenderPipelineContext& ctx) {
    OnEndFrame(ctx);
}

void RenderPipeline::EnqueuePass(RenderPipelinePass* pass) {
    _activePasses.push_back(pass);
}

void RenderPipeline::ClearPasses() noexcept {
    _activePasses.clear();
}

std::span<RenderPipelinePass*> RenderPipeline::ActivePasses() noexcept {
    return _activePasses;
}

std::span<RenderPipelinePass* const> RenderPipeline::ActivePasses() const noexcept {
    return _activePasses;
}

void RenderPipeline::OnBeginFrame(RenderPipelineContext& ctx) {
    (void)ctx;
}

void RenderPipeline::OnBuildCameraList(RenderPipelineContext& ctx, RenderCameraList& cameras) {
    (void)ctx;
    cameras.Clear();
}

void RenderPipeline::OnRender(RenderPipelineContext& ctx, const RenderCameraList& cameras) {
    PrepareTargets(ctx);

    for (const RenderCamera& camera : cameras.Cameras()) {
        OnRenderCamera(ctx, camera);
    }

    // Leave the initial clear to a camera pass when possible, so a target that
    // is about to be rendered does not need a separate clear-only render pass.
    for (RenderPipelineTarget& target : ctx.Targets) {
        if (!target.ContentDrawn) {
            const std::string_view name =
                target.Target.Window != nullptr && target.Target.Window->IsMainWindow() ? "Main Window" : "Window";
            ClearTarget(ctx, target, name);
            target.ContentDrawn = true;
        }
    }
}

void RenderPipeline::OnEndFrame(RenderPipelineContext& ctx) {
    (void)ctx;
    ClearPasses();
}

void RenderPipeline::OnRenderCamera(RenderPipelineContext& ctx, const RenderCamera& camera) {
    OnSetupCamera(ctx, camera);
    OnSetupCulling(ctx, camera);
    OnSetupLights(ctx, camera);
    OnAddRenderPasses(ctx, camera);
    OnExecutePasses(ctx, camera);
    OnFinishCamera(ctx, camera);
}

void RenderPipeline::OnSetupCamera(RenderPipelineContext& ctx, const RenderCamera& camera) {
    (void)ctx;
    (void)camera;
}

void RenderPipeline::OnSetupCulling(RenderPipelineContext& ctx, const RenderCamera& camera) {
    (void)ctx;
    (void)camera;
}

void RenderPipeline::OnSetupLights(RenderPipelineContext& ctx, const RenderCamera& camera) {
    (void)ctx;
    (void)camera;
}

void RenderPipeline::OnAddRenderPasses(RenderPipelineContext& ctx, const RenderCamera& camera) {
    (void)ctx;
    (void)camera;
}

void RenderPipeline::OnExecutePasses(RenderPipelineContext& ctx, const RenderCamera& camera) {
    std::stable_sort(_activePasses.begin(), _activePasses.end(), [](const RenderPipelinePass* lhs, const RenderPipelinePass* rhs) {
        return static_cast<int32_t>(lhs->GetRenderPassEvent()) < static_cast<int32_t>(rhs->GetRenderPassEvent());
    });

    for (RenderPipelinePass* pass : _activePasses) {
        pass->Setup(ctx, camera);
        pass->Execute(ctx, camera);
        pass->Cleanup(ctx, camera);
    }
}

void RenderPipeline::OnFinishCamera(RenderPipelineContext& ctx, const RenderCamera& camera) {
    (void)ctx;
    (void)camera;
    ClearPasses();
}

void RenderPipeline::PrepareTargets(RenderPipelineContext& ctx) {
    for (RenderPipelineTarget& target : ctx.Targets) {
        TransitionTarget(ctx, target, render::TextureState::RenderTarget);
        target.ContentDrawn = RenderTargetContent(ctx, target);
    }
}

void RenderPipeline::TransitionTarget(RenderPipelineContext& ctx, RenderPipelineTarget& target, render::TextureStates after) {
    if (target.Target.BackBuffer == nullptr || target.State == after) {
        target.State = after;
        return;
    }

    render::ResourceBarrierDescriptor barrier = render::BarrierTextureDescriptor{
        .Target = target.Target.BackBuffer,
        .Before = target.State,
        .After = after};
    ctx.Frame.GetCommandBuffer()->ResourceBarrier(std::span{&barrier, 1});
    target.State = after;
}

bool RenderPipeline::RenderTargetContent(RenderPipelineContext& ctx, RenderPipelineTarget& target) {
    if (ctx.App == nullptr || target.Target.Window == nullptr || !target.Target.Window->IsMainWindow()) {
        return false;
    }
    return ctx.App->RenderViewContent(ctx.Frame, target.Target);
}

void RenderPipeline::ClearTarget(RenderPipelineContext& ctx, RenderPipelineTarget& target, std::string_view name) {
    if (target.Target.BackBufferView == nullptr) {
        return;
    }

    GpuSystem* gpu = ctx.App != nullptr ? ctx.App->GetGpuSystem() : nullptr;
    RenderPassRegistry* registry = gpu != nullptr ? gpu->GetRenderPassRegistry() : nullptr;
    if (registry == nullptr || target.Target.BackBuffer == nullptr) {
        return;
    }
    const render::TextureDescriptor texture = target.Target.BackBuffer->GetDesc();
    render::RenderPassColorAttachmentDescriptor colorAttachment{
        .Format = texture.Format,
        .SampleCount = texture.SampleCount,
        .Load = render::LoadAction::Clear,
        .Store = render::StoreAction::Store};
    render::RenderPassDescriptor renderPassDesc{
        .ColorAttachments = std::span{&colorAttachment, 1}};
    auto passOpt = registry->GetOrCreateRenderPass(renderPassDesc);
    render::TextureView* colorView = target.Target.BackBufferView;
    auto framebufferOpt = passOpt.HasValue()
                              ? registry->GetOrCreateFramebuffer(
                                    passOpt.Get(), std::span<render::TextureView* const>{&colorView, 1},
                                    nullptr, texture.Width, texture.Height)
                              : Nullable<render::Framebuffer*>{};
    if (!passOpt.HasValue() || !framebufferOpt.HasValue()) {
        return;
    }
    const render::ColorClearValue clearValue{{0.08f, 0.10f, 0.14f, 1.0f}};
    render::RenderPassBeginDescriptor beginDesc{
        .Pass = passOpt.Get(),
        .Target = framebufferOpt.Get(),
        .ColorClearValues = std::span{&clearValue, 1},
        .Name = name};
    auto encoderOpt = ctx.Frame.GetCommandBuffer()->BeginRenderPass(beginDesc);
    if (encoderOpt.HasValue()) {
        auto encoder = encoderOpt.Release();
        ctx.Frame.GetCommandBuffer()->EndRenderPass(std::move(encoder));
    }
}

}  // namespace radray
