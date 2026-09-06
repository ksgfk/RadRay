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
    OnShutdown();
}

void RenderSystem::OnShutdown() noexcept {
    ReleaseAllScenes();
    _pipeline.reset();
    _graphRuntime.reset();
    _viewStates.reset();
    _retainedAssets.clear();
    _shaderPrograms.clear();
    _precompiledPrograms.clear();
    _shaderJit.reset();
    // 缓存的 RenderPass / Framebuffer 必须先于 GpuSystem 持有的 device 销毁。
    _renderPassRegistry.reset();
    _shaderArtifacts.clear();
    _framePlans.clear();
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
    _graphReports.resize(gpu->GetFlightDataCount());
    _renderPassRegistry = make_unique<render::RenderPassRegistry>(device);
    _graphRuntime = make_unique<RenderGraphRuntime>(*device, *_renderPassRegistry, gpu->GetFlightDataCount());
    _viewStates = make_unique<ViewStateRegistry>(*device, *_renderPassRegistry, gpu->GetFlightDataCount());
    _shaderJit = make_unique<ShaderJit>(_app->GetShaderIncludePaths());
    return {};
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

Nullable<ShaderProgram*> RenderSystem::GetOrCreateShaderProgram(
    std::span<const byte> bytes, const shader::GpuArtifactHash& expectedIdentity, const render::ShaderProgramLayoutRecipe& recipe) {
    if (!_app || !_app->GetDevice() || bytes.empty()) return nullptr;
    auto& device = *_app->GetDevice();
    const auto target = render::GetShaderTargetForBackend(device.GetBackend());
    if (!target) return nullptr;
    const shader::ShaderArtifactDecodeOptions options{.Target = *target, .ExpectedGpuArtifact = expectedIdentity};
    render::BackendShaderArtifactError error;
    auto layout = render::ResolveBackendLayoutHash(device.GetBackend(), bytes, options, recipe, &error);
    if (!layout) {
        RADRAY_ERR_LOG("Precompiled shader layout failed: {}:{}", uint32_t(error.Failure), uint32_t(error.DecodeFailure));
        return nullptr;
    }
    for (auto& value : _precompiledPrograms) {
        if (value.LayoutHash == *layout && std::ranges::equal(value.Bytes, bytes)) return value.Program.get();
    }
    PrecompiledProgram value;
    value.Bytes.assign(bytes.begin(), bytes.end());
    value.LayoutHash = *layout;
    auto artifact = render::CreateBackendShaderArtifact(device, value.Bytes, options, recipe, &error);
    if (!artifact) {
        RADRAY_ERR_LOG("Precompiled shader creation failed: {}:{}", uint32_t(error.Failure), uint32_t(error.DecodeFailure));
        return nullptr;
    }
    auto program = ShaderProgram::Create(&device, std::move(*artifact));
    if (!program) return nullptr;
    value.Program = program.Release();
    auto* result = value.Program.get();
    _precompiledPrograms.push_back(std::move(value));
    return result;
}

void RenderSystem::BeginUpdateForFlight(uint32_t flightIndex) {
    RADRAY_ASSERT(flightIndex < _retainedAssets.size());
    _retainedAssets[flightIndex].clear();
    _framePlans[flightIndex].Reset();
}

void RenderSystem::PrepareFrame(const AppUpdateContext& ctx) {
    RADRAY_ASSERT(ctx.FlightIndex < _retainedAssets.size());
    auto outputs = _outputs.GetGameThreadInfos();
    auto* windows = _app->GetWindowManager();
    for (auto& output : outputs)
        if (output.Kind == RenderOutputKind::Presentation) {
            for (size_t i = 0; i < windows->GetWindowCount(); ++i) {
                auto* window = windows->GetWindow(i);
                if (window->GetRenderOutputId() == output.Id) output.Active = output.Active && !window->IsMinimized();
            }
        }
    RenderWorkloadBuilder workloads(_framePlans[ctx.FlightIndex], outputs);
#ifdef RADRAY_ENABLE_IMGUI
    if (auto ui = _app->GetImGuiSystem()) ui->RequestOutputs(ctx.FlightIndex, workloads);
#endif
    RenderPrepareContext prepare{ctx, outputs, workloads, _retainedAssets[ctx.FlightIndex]};
    if (_pipeline)
        _pipeline->PrepareFrame(prepare);
    else
        workloads.AddPresentationOutputs();
    for (const auto& diagnostic : _framePlans[ctx.FlightIndex].Diagnostics) RADRAY_ERR_LOG("Render workload: {}", diagnostic);
}

void RenderSystem::Render(AppFrameContext& ctx) {
    if (!_graphRuntime || !_viewStates) return;
    const uint32_t flight = ctx.FlightIndex();
    const uint64_t serial = ++_frameSerial;
    auto& graphResources = _graphRuntime->BeginFlight(flight, serial, ctx.GetHostWrites());
    _viewStates->BeginFlight(flight, serial);
    auto& report = _graphReports[flight];
    report = {};
    vector<RenderSurfaceFrame> surfaces;
    struct Presentation {
        uint32_t Surface;
        AppFrameTarget Target;
    };
    vector<Presentation> presentations;
    vector<ResolvedRenderViewFamily> families;
    vector<RenderOutputInfo> resolvedOutputs;
    auto* windows = _app->GetWindowManager();
    for (const auto output : _framePlans[flight].Outputs) {
        const auto known = _outputs.Find(output);
        RenderOutputInfo info = known ? *known : RenderOutputInfo{.Id = output};
        info.Active = false;
        if (known && known->Active) {
            if (known->Kind == RenderOutputKind::ExternalColorTexture) {
                auto surface = _outputs.ResolveExternal(output);
                if (surface) {
                    surfaces.push_back(*surface);
                    info.Active = true;
                }
            } else {
                for (size_t w = 0; w < windows->GetWindowCount(); ++w) {
                    auto* window = windows->GetWindow(w);
                    if (window->GetRenderOutputId() != output || window->IsMinimized() || !window->GetSwapChain()) continue;
                    auto target = ctx.AcquireWindow(window);
                    if (!target) break;
                    const auto desc = target->BackBuffer->GetDesc();
                    presentations.push_back({static_cast<uint32_t>(surfaces.size()), *target});
                    surfaces.push_back({output, target->BackBuffer, target->BackBufferView, desc,
                                        window->GetBackBufferState(target->BackBufferIndex), render::TextureState::Present, false, false});
                    info.Width = desc.Width;
                    info.Height = desc.Height;
                    info.Format = desc.Format;
                    info.SampleCount = desc.SampleCount;
                    info.Active = true;
                    break;
                }
            }
        }
        resolvedOutputs.push_back(info);
    }
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
    if (_pipeline) _pipeline->Render(pipelineContext);
    for (auto& surface : surfaces) {
        if (!surface.Written) ClearTarget(ctx, surface);
        TransitionSurface(ctx, surface, surface.RequiredFinalState);
        _outputs.CommitExternalState(surface);
    }
    for (const auto& presentation : presentations) {
        presentation.Target.Window->SetBackBufferState(presentation.Target.BackBufferIndex, surfaces[presentation.Surface].CurrentState);
    }
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
