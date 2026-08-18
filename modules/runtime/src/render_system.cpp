#include <radray/runtime/render_system.h>

#include <algorithm>
#include <optional>
#include <span>
#include <utility>

#include <radray/file.h>
#include <radray/hash.h>
#include <radray/logger.h>
#include <radray/render/backend_shader_artifact.h>
#include <radray/render/rhi.h>
#include <radray/runtime/application.h>
#include <radray/runtime/gpu_system.h>
#include <radray/runtime/render_framework/scene.h>
#include <radray/runtime/shader_jit.h>
#include <radray/runtime/shader_program.h>
#include <radray/runtime/window_manager.h>

namespace radray {

RenderSystem::RenderSystem(Application* app) noexcept
    : _app(app) {
}

RenderSystem::~RenderSystem() noexcept {
    ReleaseAllScenes();
    _pipeline.reset();
    _shaderPrograms.clear();
    _shaderJit.reset();
    // 缓存的 RenderPass / Framebuffer 必须先于 GpuSystem 持有的 device 销毁。
    _renderPassRegistry.reset();
}

void RenderSystem::OnInitialize() {
    GpuSystem* gpu = _app != nullptr ? _app->GetGpuSystem() : nullptr;
    render::Device* device = _app != nullptr ? _app->GetDevice() : nullptr;
    if (gpu == nullptr || device == nullptr) {
        RADRAY_ERR_LOG("RenderSystem::OnInitialize: GpuSystem or Device is null");
        return;
    }

    _renderPassRegistry = make_unique<render::RenderPassRegistry>(device);
    _shaderJit = make_unique<ShaderJit>(_app->GetShaderIncludePaths());
}

size_t RenderSystem::ProgramKeyHash::operator()(const ProgramKey& value) const noexcept {
    HashCode hash;
    hash.Add(value.SourceName);
    hash.Add(value.Assignments.size());
    for (const ProgramAssignment& assignment : value.Assignments) {
        hash.Add(assignment.Name);
        hash.Add(assignment.Value);
    }
    return hash.ToHashCode();
}

Nullable<ShaderProgram*> RenderSystem::GetOrCreateShaderProgram(
    std::string_view sourceName,
    std::span<const shader::KeywordAssignment> assignments,
    const render::ShaderLayoutPolicy& layoutPolicy,
    const shader::CompilePolicy& compilePolicy) {
    ProgramKey key{.SourceName = string{sourceName}};
    key.Assignments.reserve(assignments.size());
    for (const shader::KeywordAssignment& assignment : assignments) {
        key.Assignments.push_back(ProgramAssignment{
            .Name = assignment.Name,
            .Value = assignment.Value});
    }
    std::sort(
        key.Assignments.begin(),
        key.Assignments.end(),
        [](const ProgramAssignment& lhs, const ProgramAssignment& rhs) {
            return std::tie(lhs.Name, lhs.Value) < std::tie(rhs.Name, rhs.Value);
        });

    auto [cacheIt, inserted] = _shaderPrograms.try_emplace(std::move(key), nullptr);
    if (!inserted) {
        return cacheIt->second.get();
    }

    for (size_t index = 1; index < cacheIt->first.Assignments.size(); ++index) {
        if (cacheIt->first.Assignments[index - 1].Name ==
            cacheIt->first.Assignments[index].Name) {
            RADRAY_ERR_LOG(
                "shader program '{}' has duplicate keyword assignment '{}'",
                sourceName,
                cacheIt->first.Assignments[index].Name);
            return nullptr;
        }
    }
    if (_app == nullptr || _app->GetDevice() == nullptr || _shaderJit == nullptr ||
        !_shaderJit->IsAvailable()) {
        RADRAY_ERR_LOG("shader program '{}' unavailable: shader JIT is disabled or unavailable", sourceName);
        return nullptr;
    }
    if (!shader::IsLogicalSourceName(sourceName) || _app->GetShaderSourceRoot().empty()) {
        RADRAY_ERR_LOG("shader program '{}' unavailable: invalid source name or empty source root", sourceName);
        return nullptr;
    }

    const std::filesystem::path sourcePath = _app->GetShaderSourceRoot() / std::filesystem::path{sourceName};
    std::optional<vector<byte>> source = ReadBinaryFile(sourcePath);
    if (!source.has_value() || source->empty()) {
        RADRAY_ERR_LOG("shader program source read failed: {}", sourcePath.string());
        return nullptr;
    }
    const std::optional<shader::ShaderTarget> target =
        render::GetShaderTargetForBackend(_app->GetDevice()->GetBackend());
    if (!target.has_value()) {
        RADRAY_ERR_LOG("shader program '{}' has no target for the active backend", sourceName);
        return nullptr;
    }
    const std::optional<shader::ContractHash> contract = _shaderJit->DiscoverContractHash(
        sourceName,
        source.value(),
        target.value());
    if (!contract.has_value()) {
        RADRAY_ERR_LOG("shader program '{}' contract discovery failed", sourceName);
        return nullptr;
    }

    shader::CompileVariantRequest request{
        .SourceName = string{sourceName},
        .RootSource = std::move(source.value()),
        .Targets = static_cast<shader::ShaderTargetMask>(shader::ToTargetMask(target.value())),
        .Policy = compilePolicy,
        .ExpectedContract = contract.value()};
    request.Assignments.reserve(cacheIt->first.Assignments.size());
    for (const ProgramAssignment& assignment : cacheIt->first.Assignments) {
        request.Assignments.push_back(shader::KeywordAssignment{
            .Name = assignment.Name,
            .Value = assignment.Value});
    }
    const std::optional<ShaderJitArtifact> compiled =
        _shaderJit->Compile(request, target.value());
    if (!compiled.has_value()) {
        RADRAY_ERR_LOG("shader program '{}' compilation failed", sourceName);
        return nullptr;
    }

    render::BackendShaderArtifactError artifactError;
    std::optional<render::BackendShaderArtifact> artifact =
        render::CreateBackendShaderArtifact(
            *_app->GetDevice(),
            compiled->Metadata,
            shader::ShaderArtifactDecodeOptions{
                .Target = compiled->Target,
                .ExpectedGpuArtifact = compiled->ExpectedGpuArtifact},
            layoutPolicy,
            &artifactError);
    if (!artifact.has_value()) {
        RADRAY_ERR_LOG(
            "shader program '{}' artifact creation failed: {}:{}",
            sourceName,
            static_cast<uint32_t>(artifactError.Failure),
            static_cast<uint32_t>(artifactError.DecodeFailure));
        return nullptr;
    }
    Nullable<unique_ptr<ShaderProgram>> program =
        ShaderProgram::Create(
            _app->GetDevice(),
            std::move(artifact.value()),
            layoutPolicy);
    if (!program.HasValue()) {
        RADRAY_ERR_LOG("shader program '{}' GPU object creation failed", sourceName);
        return nullptr;
    }
    cacheIt->second = program.Release();
    return cacheIt->second.get();
}

void RenderSystem::SetPipeline(unique_ptr<RenderPipeline> pipeline) noexcept {
    _pipeline = std::move(pipeline);
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
