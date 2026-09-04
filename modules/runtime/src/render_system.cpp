#include <radray/runtime/render_system.h>

#include <algorithm>
#include <cstring>
#include <optional>
#include <type_traits>
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
    _retainedAssets.clear();
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

    _retainedAssets.resize(gpu->GetFlightDataCount());
    _renderPassRegistry = make_unique<render::RenderPassRegistry>(device);
    _shaderJit = make_unique<ShaderJit>(_app->GetShaderIncludePaths());
}

namespace {

// Canonical form of a name/value list: sorted, so the caller's order does not change identity, and
// duplicate names are rejected instead of being silently merged.
template <typename Entry, typename Source>
bool CanonicalizeNamedValues(
    std::span<const Source> source,
    vector<Entry>& out,
    std::string_view what,
    std::string_view sourceName) {
    out.reserve(source.size());
    for (const Source& value : source) {
        out.push_back(Entry{.Name = value.Name, .Value = value.Value});
    }
    std::sort(out.begin(), out.end(), [](const Entry& lhs, const Entry& rhs) {
        return std::tie(lhs.Name, lhs.Value) < std::tie(rhs.Name, rhs.Value);
    });
    for (size_t index = 1; index < out.size(); ++index) {
        if (out[index - 1].Name == out[index].Name) {
            RADRAY_ERR_LOG(
                "shader program '{}' has duplicate {} '{}'",
                sourceName,
                what,
                out[index].Name);
            return false;
        }
    }
    return true;
}

// Hashes the raw bytes of a trivially copyable value. The compile policy is a fixed-size POD whose
// size is static_asserted, so hashing its bytes cannot miss a field that a later change adds.
template <typename T>
void AddValueBytes(HashCode& hash, const T& value) noexcept {
    static_assert(std::is_trivially_copyable_v<T>);
    array<uint8_t, sizeof(T)> bytes{};
    std::memcpy(bytes.data(), &value, sizeof(T));
    for (const uint8_t element : bytes) {
        hash.Add(element);
    }
}

}  // namespace

size_t RenderSystem::ArtifactKeyHash::operator()(const ArtifactKey& value) const noexcept {
    HashCode hash;
    hash.Add(value.SourceName);
    hash.Add(value.Defines.size());
    for (const ProgramText& define : value.Defines) {
        hash.Add(define.Name);
        hash.Add(define.Value);
    }
    hash.Add(value.Assignments.size());
    for (const ProgramText& assignment : value.Assignments) {
        hash.Add(assignment.Name);
        hash.Add(assignment.Value);
    }
    // The whole policy takes part: two shader models or two warning policies are two artifacts.
    AddValueBytes(hash, value.Policy);
    AddValueBytes(hash, value.Target);
    AddValueBytes(hash, value.Toolchain.Bytes);
    return hash.ToHashCode();
}

size_t RenderSystem::ProgramKeyHash::operator()(const ProgramKey& value) const noexcept {
    HashCode hash;
    hash.Add(value.ArtifactIdentity);
    AddValueBytes(hash, value.LayoutHash.Bytes);
    return hash.ToHashCode();
}

// Compiles once per artifact key and remembers the outcome, success or failure, under that key. The
// returned record is owned by the cache; it stays valid until the cache is cleared.
Nullable<const RenderSystem::ArtifactRecord*> RenderSystem::GetOrCompileArtifact(
    const ShaderProgramRequest& request,
    ArtifactKey key) {
    const auto cached = _shaderArtifacts.find(key);
    if (cached != _shaderArtifacts.end()) {
        return cached->second.Failed ? nullptr : &cached->second;
    }

    const auto fail = [&]() -> Nullable<const ArtifactRecord*> {
        _shaderArtifacts.emplace(std::move(key), ArtifactRecord{.Failed = true});
        return nullptr;
    };

    if (!shader::IsLogicalSourceName(request.SourceName) ||
        _app->GetShaderSourceRoot().empty()) {
        RADRAY_ERR_LOG(
            "shader program '{}' unavailable: invalid source name or empty source root",
            request.SourceName);
        return fail();
    }
    const std::filesystem::path sourcePath =
        _app->GetShaderSourceRoot() / std::filesystem::path{request.SourceName};
    std::optional<vector<byte>> source = ReadBinaryFile(sourcePath);
    if (!source.has_value() || source->empty()) {
        RADRAY_ERR_LOG("shader program source read failed: {}", sourcePath.string());
        return fail();
    }

    // Discovery and compilation are driven from the same inputs. A contract discovered under a
    // different policy or a different define set can describe a different set of entry points, and
    // the compile would then be checked against the wrong contract.
    shader::SourceContractRequest discovery{
        .SourceName = request.SourceName,
        .RootSource = source.value(),
        .Defines = request.Defines,
        .Targets = static_cast<shader::ShaderTargetMask>(shader::ToTargetMask(key.Target)),
        .Policy = request.Policy};
    const std::optional<shader::ContractHash> contract =
        _shaderJit->DiscoverContractHash(discovery);
    if (!contract.has_value()) {
        RADRAY_ERR_LOG("shader program '{}' contract discovery failed", request.SourceName);
        return fail();
    }

    shader::CompileVariantRequest compile{
        .SourceName = request.SourceName,
        .RootSource = std::move(source.value()),
        .Defines = request.Defines,
        .Assignments = request.Assignments,
        .Targets = static_cast<shader::ShaderTargetMask>(shader::ToTargetMask(key.Target)),
        .Policy = request.Policy,
        .ExpectedContract = contract.value()};
    std::optional<ShaderJitArtifact> compiled = _shaderJit->Compile(compile, key.Target);
    if (!compiled.has_value()) {
        RADRAY_ERR_LOG("shader program '{}' compilation failed", request.SourceName);
        return fail();
    }

    ArtifactRecord record{
        .Failed = false,
        .Identity = _nextArtifactIdentity++,
        .Artifact = std::move(compiled.value())};
    const auto inserted = _shaderArtifacts.emplace(std::move(key), std::move(record));
    return &inserted.first->second;
}

Nullable<ShaderProgram*> RenderSystem::GetOrCreateShaderProgram(
    const ShaderProgramRequest& request) {
    if (_app == nullptr || _app->GetDevice() == nullptr || _shaderJit == nullptr ||
        !_shaderJit->IsAvailable()) {
        RADRAY_ERR_LOG(
            "shader program '{}' unavailable: shader JIT is disabled or unavailable",
            request.SourceName);
        return nullptr;
    }
    const std::optional<shader::ShaderTarget> target =
        render::GetShaderTargetForBackend(_app->GetDevice()->GetBackend());
    if (!target.has_value()) {
        RADRAY_ERR_LOG(
            "shader program '{}' has no target for the active backend",
            request.SourceName);
        return nullptr;
    }
    // Part of the artifact identity: the same source compiled by another toolchain is another
    // artifact, and nothing in the request would say so.
    const std::optional<shader::Hash128> toolchain = _shaderJit->GetToolchainIdentity();
    if (!toolchain.has_value()) {
        RADRAY_ERR_LOG(
            "shader program '{}' unavailable: the compiler reports no toolchain identity",
            request.SourceName);
        return nullptr;
    }

    // A malformed request is the caller's bug rather than a property of a shader, so it is reported
    // and not remembered under any key.
    ArtifactKey artifactKey{
        .SourceName = request.SourceName,
        .Policy = request.Policy,
        .Target = target.value(),
        .Toolchain = toolchain.value()};
    if (!CanonicalizeNamedValues<ProgramText>(
            std::span{request.Defines},
            artifactKey.Defines,
            "define",
            request.SourceName) ||
        !CanonicalizeNamedValues<ProgramText>(
            std::span{request.Assignments},
            artifactKey.Assignments,
            "keyword assignment",
            request.SourceName)) {
        return nullptr;
    }

    const Nullable<const ArtifactRecord*> artifactRecord =
        GetOrCompileArtifact(request, std::move(artifactKey));
    if (!artifactRecord.HasValue()) {
        return nullptr;
    }
    const ShaderJitArtifact& compiled = artifactRecord.Get()->Artifact;
    const shader::ShaderArtifactDecodeOptions decodeOptions{
        .Target = compiled.Target,
        .ExpectedGpuArtifact = compiled.ExpectedGpuArtifact};

    // The program identity is the artifact plus the resolved layout of the active backend only, so a
    // recipe change that the active backend does not see resolves to the same hash and reuses both
    // the artifact and the program.
    render::BackendShaderArtifactError artifactError;
    const std::optional<render::ResolvedLayoutHash> layoutHash =
        render::ResolveBackendLayoutHash(
            _app->GetDevice()->GetBackend(),
            compiled.Metadata,
            decodeOptions,
            request.LayoutRecipe,
            &artifactError);
    if (!layoutHash.has_value()) {
        RADRAY_ERR_LOG(
            "shader program '{}' layout resolve failed: {}:{}",
            request.SourceName,
            static_cast<uint32_t>(artifactError.Failure),
            static_cast<uint32_t>(artifactError.DecodeFailure));
        return nullptr;
    }

    const ProgramKey programKey{
        .ArtifactIdentity = artifactRecord.Get()->Identity,
        .LayoutHash = layoutHash.value()};
    const auto cached = _shaderPrograms.find(programKey);
    if (cached != _shaderPrograms.end()) {
        return cached->second.Failed ? nullptr : cached->second.Program.get();
    }

    std::optional<render::BackendShaderArtifact> artifact =
        render::CreateBackendShaderArtifact(
            *_app->GetDevice(),
            compiled.Metadata,
            decodeOptions,
            request.LayoutRecipe,
            &artifactError);
    if (!artifact.has_value()) {
        RADRAY_ERR_LOG(
            "shader program '{}' artifact creation failed: {}:{}",
            request.SourceName,
            static_cast<uint32_t>(artifactError.Failure),
            static_cast<uint32_t>(artifactError.DecodeFailure));
        _shaderPrograms.emplace(programKey, ProgramRecord{.Failed = true});
        return nullptr;
    }
    Nullable<unique_ptr<ShaderProgram>> program =
        ShaderProgram::Create(_app->GetDevice(), std::move(artifact.value()));
    if (!program.HasValue()) {
        RADRAY_ERR_LOG("shader program '{}' GPU object creation failed", request.SourceName);
        _shaderPrograms.emplace(programKey, ProgramRecord{.Failed = true});
        return nullptr;
    }
    const auto inserted = _shaderPrograms.emplace(
        programKey,
        ProgramRecord{.Failed = false, .Program = program.Release()});
    return inserted.first->second.Program.get();
}

void RenderSystem::SetPipeline(unique_ptr<RenderPipeline> pipeline) noexcept {
    _pipeline = std::move(pipeline);
}

void RenderSystem::BeginUpdateForFlight(uint32_t flightIndex) {
    RADRAY_ASSERT(flightIndex < _retainedAssets.size());
    _retainedAssets[flightIndex].clear();
}

void RenderSystem::PrepareFrame(const AppUpdateContext& ctx) {
    RADRAY_ASSERT(ctx.FlightIndex < _retainedAssets.size());
    if (_pipeline != nullptr) {
        _pipeline->PrepareFrame(ctx, _retainedAssets[ctx.FlightIndex]);
    }
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

    for (RenderPipelineTarget& target : targets) {
        EnsureRenderTargetState(ctx, target);
    }
    if (_pipeline != nullptr) {
        RenderPipelineContext pipelineCtx{ctx, targets};
        _pipeline->Render(pipelineCtx);
    }
    for (RenderPipelineTarget& target : targets) {
        if (!target.ContentDrawn) {
            ClearTarget(ctx, target);
        }
        EnsurePresentState(ctx, target);
    }
}

void RenderSystem::ClearTarget(AppFrameContext& ctx, RenderPipelineTarget& target) {
    if (target.Target.BackBufferView == nullptr) {
        return;
    }

    render::RenderPassRegistry* registry = _renderPassRegistry.get();
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
    auto framebufferOpt = Nullable<render::Framebuffer*>{};
    if (passOpt.HasValue()) {
        const render::FramebufferDescriptor framebufferDesc{
            .Pass = passOpt.Get(),
            .ColorAttachments = std::span<render::TextureView* const>{&colorView, 1},
            .DepthStencilAttachment = nullptr,
            .Width = texture.Width,
            .Height = texture.Height};
        framebufferOpt = registry->GetOrCreateFramebuffer(framebufferDesc);
    }
    if (!passOpt.HasValue() || !framebufferOpt.HasValue()) {
        return;
    }
    const render::ColorClearValue clearValue{{0.08f, 0.10f, 0.14f, 1.0f}};
    render::RenderPassBeginDescriptor beginDesc{
        .Pass = passOpt.Get(),
        .Target = framebufferOpt.Get(),
        .ColorClearValues = std::span{&clearValue, 1},
        .Name = "Fallback Clear"};
    auto encoderOpt = ctx.GetCommandBuffer()->BeginRenderPass(beginDesc);
    if (encoderOpt.HasValue()) {
        auto encoder = encoderOpt.Release();
        ctx.GetCommandBuffer()->EndRenderPass(std::move(encoder));
        target.ContentDrawn = true;
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
