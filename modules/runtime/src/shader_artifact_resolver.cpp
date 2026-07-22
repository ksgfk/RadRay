#include <radray/runtime/shader_artifact_resolver.h>

#include <radray/runtime/asset_manager.h>
#include <radray/runtime/shader_asset.h>

#if defined(RADRAY_ENABLE_SHADER_JIT)
#include <radray/render/dxc.h>
#endif

#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include <fmt/format.h>

#include <radray/binary_io.h>
#include <radray/hash.h>
#if defined(RADRAY_ENABLE_SPIRV_CROSS)
#include <radray/render/spvc.h>
#endif

namespace radray {
namespace {

using ShaderFuture = ShaderArtifactResolver::Future;
using StageFuture = std::shared_future<ShaderJitStageCompileResult>;

struct ProgramJob {
    render::ShaderPassDesc Pass;
    uint32_t PassIndex{0};
    render::ShaderTarget Target{render::ShaderTarget::DXIL};
    vector<string> Defines;
    ShaderSourceIdentity Source;
    std::optional<render::ShaderInterfaceDesc> ExpectedInterface;
    render::ShaderHash ProgramIdentity{};
    render::ShaderHash ProgramKey{};
};

struct ShaderAssetPassKey {
    AssetHandle Handle{AssetHandle::Invalid()};
    uint32_t PassIndex{0};

    friend bool operator==(const ShaderAssetPassKey&, const ShaderAssetPassKey&) = default;
};

struct ShaderAssetPassKeyHasher {
    size_t operator()(const ShaderAssetPassKey& value) const noexcept {
        size_t result = std::hash<uint32_t>{}(value.Handle.Index);
        const auto combine = [&result](uint32_t part) {
            const size_t valueHash = std::hash<uint32_t>{}(part);
            result ^= valueHash + 0x9e3779b9u + (result << 6) + (result >> 2);
        };
        combine(value.Handle.Generation);
        combine(value.PassIndex);
        return result;
    }
};

bool IsKnownTarget(render::ShaderTarget target) noexcept {
    return target == render::ShaderTarget::DXIL || target == render::ShaderTarget::SPIRV;
}

vector<render::ShaderStage> GetPassStages(const render::ShaderPassDesc& pass) {
    if (const auto* graphics = std::get_if<render::ShaderGraphicsPassDesc>(&pass.Program)) {
        vector<render::ShaderStage> result{render::ShaderStage::Vertex};
        if (graphics->PixelEntry.has_value()) result.emplace_back(render::ShaderStage::Pixel);
        return result;
    }
    return {render::ShaderStage::Compute};
}

void WriteHash(BinaryWriter& output, render::ShaderHash value) {
    output.U64(value.Low);
    output.U64(value.High);
}

void WritePassCompileOptions(BinaryWriter& output, const render::ShaderPassDesc& pass) {
    output.U32(static_cast<uint32_t>(pass.SM));
    output.Bool(pass.IsOptimize);
    output.Bool(pass.EnableUnbounded);
    if (const auto* graphics = std::get_if<render::ShaderGraphicsPassDesc>(&pass.Program)) {
        output.U8(0);
        output.String(graphics->VertexEntry);
        output.String(graphics->PixelEntry.value_or(string{}));
    } else {
        output.U8(1);
        output.String(std::get<render::ShaderComputePassDesc>(pass.Program).EntryPoint);
    }
}

render::ShaderHash BuildProgramKey(
    const ProgramJob& job,
    render::ShaderHash toolchain) {
    BinaryWriter bytes;
    bytes.String("radray-jit-program-cache-v2");
    WriteHash(bytes, job.ProgramIdentity);
    WriteHash(bytes, toolchain);
    bytes.U8(static_cast<uint8_t>(job.Target));
    return render::HashShaderBytes(bytes.GetData());
}

render::ShaderHash BuildStageKey(
    const ProgramJob& job,
    render::ShaderStage stage,
    const vector<string>& projectedDefines,
    render::ShaderHash toolchain) {
    BinaryWriter bytes;
    bytes.String("radray-jit-stage-v1");
    WriteHash(bytes, job.Source.Hash);
    WriteHash(bytes, toolchain);
    bytes.U32(job.PassIndex);
    bytes.U8(static_cast<uint8_t>(job.Target));
    bytes.U32(static_cast<uint32_t>(stage));
    WritePassCompileOptions(bytes, job.Pass);
    bytes.Size32(projectedDefines.size());
    for (const string& define : projectedDefines) bytes.String(define);
    return render::HashShaderBytes(bytes.GetData());
}

std::filesystem::path GetDiskPath(
    const ShaderArtifactResolver::Config& config,
    render::ShaderHash key) {
    return config.DiskCacheDirectory /
           fmt::format("{:016x}{:016x}.rrjit", key.High, key.Low);
}

render::ShaderDiagnostic MakeDiagnostic(
    render::ShaderDiagnosticCode code,
    string message,
    const ProgramJob& job,
    render::ShaderStage stage = render::ShaderStage::UNKNOWN) {
    return render::ShaderDiagnostic{
        .Code = code,
        .Message = std::move(message),
        .Context = render::ShaderDiagnosticContext{
            .Target = job.Target,
            .PassIndex = job.PassIndex,
            .VariantDefines = job.Defines,
            .Stage = stage}};
}

template <typename T>
std::shared_future<T> MakeReadyFuture(T value) {
    std::promise<T> promise;
    promise.set_value(std::move(value));
    return promise.get_future().share();
}

bool ValidateCompiledStage(
    const ShaderJitStageCompileRequest& request,
    ShaderJitStageCompileResult& result) {
    if (!result.Artifact.has_value()) return false;
    const ShaderResolvedStageArtifact& artifact = *result.Artifact;
    const auto expectedEntry = render::FindShaderEntryPoint(request.Pass, request.Stage);
    const render::ShaderDiagnosticContext context{
        .Target = request.Target,
        .PassIndex = request.PassIndex,
        .VariantDefines = request.FullDefines,
        .Stage = request.Stage};
    const auto fail = [&](render::ShaderDiagnosticCode code, string message) {
        result.Artifact.reset();
        result.Diagnostics.emplace_back(render::ShaderDiagnostic{
            .Code = code,
            .Message = std::move(message),
            .Context = context});
        return false;
    };
    if (!expectedEntry.has_value() || artifact.Target != request.Target ||
        artifact.PassIndex != request.PassIndex || artifact.Stage != request.Stage ||
        artifact.Defines != request.ProjectedDefines || artifact.EntryPoint != *expectedEntry) {
        return fail(
            render::ShaderDiagnosticCode::CompilationFailed,
            "JIT compiler returned stage metadata that does not match the request");
    }
    if (artifact.Bytecode.empty() || artifact.BinaryHash == render::ShaderHash{} ||
        artifact.BinaryHash != render::HashShaderBytes(artifact.Bytecode)) {
        return fail(
            render::ShaderDiagnosticCode::CompilationFailed,
            "JIT compiler returned empty bytecode or an invalid bytecode hash");
    }

    render::ShaderInterfaceNormalizationOptions options{
        .Context = context,
        .PushConstantBindings = {}};
    render::ShaderStageInterfaceBuildResult normalized;
    if (request.Target == render::ShaderTarget::DXIL) {
        const auto* reflection = std::get_if<render::HlslShaderDesc>(&artifact.Reflection);
        if (reflection == nullptr) {
            return fail(
                render::ShaderDiagnosticCode::InvalidReflection,
                "DXIL JIT artifact did not contain HLSL reflection");
        }
        normalized = render::NormalizeHlslInterface(*reflection, request.Stage, options);
    } else {
        const auto* reflection = std::get_if<render::SpirvShaderDesc>(&artifact.Reflection);
        if (reflection == nullptr) {
            return fail(
                render::ShaderDiagnosticCode::InvalidReflection,
                "SPIR-V JIT artifact did not contain SPIR-V reflection");
        }
        normalized = render::NormalizeSpirvInterface(*reflection, request.Stage, options);
    }
    if (!normalized.Succeeded()) {
        result.Artifact.reset();
        result.Diagnostics.insert(
            result.Diagnostics.end(),
            std::make_move_iterator(normalized.Diagnostics.begin()),
            std::make_move_iterator(normalized.Diagnostics.end()));
        return false;
    }
    if (*normalized.Interface != artifact.Interface) {
        return fail(
            render::ShaderDiagnosticCode::InterfaceMismatch,
            "JIT compiler reflection and canonical stage interface disagree");
    }
    return true;
}

ShaderArtifactResolutionResult ResolveBaked(
    const render::ShaderBinary& binary,
    const render::ShaderProgramVariantArtifact& program,
    uint32_t originalPassIndex) {
    auto resolved = make_shared<ShaderResolvedProgram>();
    resolved->Target = program.Target;
    resolved->PassIndex = originalPassIndex;
    resolved->Defines = program.Defines;
    resolved->Interface = binary.ProgramInterfaces[program.InterfaceIndex];
    const render::ShaderPassDesc& pass = binary.Asset.Passes[program.PassIndex];
    resolved->SourceIdentity = pass.SourceIdentity;
    resolved->ProgramIdentity = ComputeShaderProgramIdentity(
        pass,
        originalPassIndex,
        program.Defines,
        pass.SourceIdentity);
    for (uint32_t artifactIndex : program.StageArtifactIndices) {
        const render::ShaderStageArtifact& artifact = binary.StageArtifacts[artifactIndex];
        resolved->Stages.emplace_back(ShaderResolvedStageArtifact{
            .Target = artifact.Target,
            .PassIndex = originalPassIndex,
            .Stage = artifact.Stage,
            .Defines = artifact.Defines,
            .EntryPoint = artifact.EntryPoint,
            .Bytecode = artifact.Bytecode,
            .BinaryHash = artifact.BinaryHash,
            .Reflection = binary.Reflections[artifact.ReflectionIndex].Reflection,
            .Interface = binary.StageInterfaces[artifact.InterfaceIndex]});
    }
    return ShaderArtifactResolutionResult{
        .Program = std::move(resolved),
        .Diagnostics = {},
        .Source = ShaderArtifactSource::Baked};
}

std::optional<string> SerializeReflection(const render::ShaderReflectionDesc& reflection) {
    if (const auto* hlsl = std::get_if<render::HlslShaderDesc>(&reflection)) {
        return render::SerializeHlslShaderDesc(*hlsl);
    }
    return render::SerializeSpirvShaderDesc(std::get<render::SpirvShaderDesc>(reflection));
}

uint32_t InternReflection(
    render::ShaderBinary& binary,
    render::ShaderTarget target,
    const render::ShaderReflectionDesc& reflection) {
    const auto payload = SerializeReflection(reflection);
    if (!payload.has_value()) return render::kInvalidShaderTableIndex;
    const render::ShaderHash hash = render::HashShaderBytes(
        std::as_bytes(std::span{payload->data(), payload->size()}));
    for (uint32_t i = 0; i < binary.Reflections.size(); ++i) {
        const auto existing = SerializeReflection(binary.Reflections[i].Reflection);
        if (binary.Reflections[i].Target == target && existing.has_value() && *existing == *payload) return i;
    }
    binary.Reflections.emplace_back(render::ShaderReflectionRecord{
        .Target = target,
        .Reflection = reflection,
        .Hash = hash});
    return static_cast<uint32_t>(binary.Reflections.size() - 1);
}

template <typename T>
uint32_t InternValue(vector<T>& values, const T& value) {
    const auto it = std::ranges::find(values, value);
    if (it != values.end()) return static_cast<uint32_t>(std::distance(values.begin(), it));
    values.emplace_back(value);
    return static_cast<uint32_t>(values.size() - 1);
}

Guid GuidFromHash(render::ShaderHash hash) noexcept {
    Guid::ByteArray bytes{};
    static_assert(sizeof(hash) == bytes.size());
    std::memcpy(bytes.data(), &hash, bytes.size());
    if (std::ranges::all_of(bytes, [](uint8_t value) { return value == 0; })) bytes[0] = 1;
    return Guid{bytes};
}

render::ShaderBinary BuildDiskBinary(
    const ProgramJob& job,
    const ShaderResolvedProgram& program) {
    render::ShaderBinary binary;
    binary.Asset.AssetId = GuidFromHash(job.ProgramKey);
    render::ShaderPassDesc pass = job.Pass;
    pass.SourceIdentity = job.Source.Hash;
    pass.BakeSet.Variants = {render::ShaderVariantKey{.Defines = job.Defines}};
    binary.Asset.Passes = {std::move(pass)};
    binary.ProgramInterfaces = {program.Interface};
    vector<uint32_t> stageIndices;
    for (const ShaderResolvedStageArtifact& stage : program.Stages) {
        const uint32_t reflectionIndex = InternReflection(binary, stage.Target, stage.Reflection);
        if (reflectionIndex == render::kInvalidShaderTableIndex) return {};
        const uint32_t interfaceIndex = InternValue(binary.StageInterfaces, stage.Interface);
        binary.StageArtifacts.emplace_back(render::ShaderStageArtifact{
            .Target = stage.Target,
            .Category = render::GetShaderBlobCategory(stage.Target),
            .PassIndex = 0,
            .Stage = stage.Stage,
            .Defines = stage.Defines,
            .EntryPoint = stage.EntryPoint,
            .Bytecode = stage.Bytecode,
            .BinaryHash = stage.BinaryHash,
            .ReflectionIndex = reflectionIndex,
            .InterfaceIndex = interfaceIndex});
        stageIndices.emplace_back(static_cast<uint32_t>(binary.StageArtifacts.size() - 1));
    }
    binary.ProgramVariants.emplace_back(render::ShaderProgramVariantArtifact{
        .Target = program.Target,
        .PassIndex = 0,
        .Defines = program.Defines,
        .StageArtifactIndices = std::move(stageIndices),
        .InterfaceIndex = 0});
    return binary;
}

std::optional<ShaderArtifactResolutionResult> ReadDiskResult(
    const ShaderArtifactResolver::Config& config,
    const ProgramJob& job) {
    if (!config.EnableDiskCache || config.DiskCacheDirectory.empty()) return std::nullopt;
    auto binary = render::ReadShaderBinary(GetDiskPath(config, job.ProgramKey));
    render::ShaderPassDesc expectedPass = job.Pass;
    expectedPass.SourceIdentity = job.Source.Hash;
    expectedPass.BakeSet.Variants = {render::ShaderVariantKey{.Defines = job.Defines}};
    if (!binary.has_value() || binary->Asset.AssetId != GuidFromHash(job.ProgramKey) ||
        binary->Asset.Passes.size() != 1 || binary->Asset.Passes.front() != expectedPass) {
        return std::nullopt;
    }
    auto program = binary->FindProgramVariant(job.Target, 0, job.Defines);
    if (!program.HasValue()) return std::nullopt;
    ShaderArtifactResolutionResult result = ResolveBaked(*binary, *program.Get(), job.PassIndex);
    if (result.Program->ProgramIdentity != job.ProgramIdentity) return std::nullopt;
    auto mutableProgram = make_shared<ShaderResolvedProgram>(*result.Program);
    mutableProgram->SourceIdentity = job.Source.Hash;
    mutableProgram->ProgramIdentity = job.ProgramIdentity;
    result.Program = std::move(mutableProgram);
    result.Source = ShaderArtifactSource::DiskCache;
    return result;
}

bool WriteDiskResult(
    const ShaderArtifactResolver::Config& config,
    const ProgramJob& job,
    const ShaderResolvedProgram& program) {
    if (!config.EnableDiskCache || config.DiskCacheDirectory.empty()) return true;
    render::ShaderBinary binary = BuildDiskBinary(job, program);
    return binary.IsValid() && render::WriteShaderBinary(GetDiskPath(config, job.ProgramKey), binary);
}

}  // namespace

struct ShaderArtifactResolver::State {
    AssetManager* Assets{nullptr};
    Config ConfigValue;
    shared_ptr<IShaderJitCompiler> Compiler;
    mutable std::mutex ProgramMutex;
    mutable std::mutex StageMutex;
    mutable std::mutex SourceMutex;
    mutable std::mutex CanonicalMutex;
    mutable std::mutex CompilerMutex;
    mutable std::mutex DiskMutex;
    unordered_map<render::ShaderHash, ShaderFuture, PodHasher<render::ShaderHash>, PodEqual<render::ShaderHash>> Programs;
    unordered_map<render::ShaderHash, StageFuture, PodHasher<render::ShaderHash>, PodEqual<render::ShaderHash>> Stages;
    unordered_map<ShaderAssetPassKey, ShaderSourceIdentity, ShaderAssetPassKeyHasher> AssetSources;
    unordered_map<render::ShaderHash, render::ShaderInterfaceDesc, PodHasher<render::ShaderHash>, PodEqual<render::ShaderHash>> CanonicalPrograms;
};

namespace {

void PruneInvalidAssetSourcesLocked(ShaderArtifactResolver::State& state) {
    std::erase_if(
        state.AssetSources,
        [&state](const auto& entry) {
            return state.Assets == nullptr || !state.Assets->IsHandleValid(entry.first.Handle);
        });
}

bool ValidateCanonicalInterface(
    const shared_ptr<ShaderArtifactResolver::State>& state,
    const ProgramJob& job,
    const render::ShaderInterfaceDesc& interface,
    vector<render::ShaderDiagnostic>& diagnostics) {
    if (job.ExpectedInterface.has_value() && *job.ExpectedInterface != interface) {
        diagnostics.emplace_back(MakeDiagnostic(
            render::ShaderDiagnosticCode::InterfaceMismatch,
            "JIT program interface is incompatible with the baked interface for another target",
            job));
        return false;
    }
    std::scoped_lock lock{state->CanonicalMutex};
    const auto existing = state->CanonicalPrograms.find(job.ProgramIdentity);
    if (existing != state->CanonicalPrograms.end() && existing->second != interface) {
        diagnostics.emplace_back(MakeDiagnostic(
            render::ShaderDiagnosticCode::InterfaceMismatch,
            "JIT targets produced different canonical program interfaces",
            job));
        return false;
    }
    if (existing == state->CanonicalPrograms.end()) {
        state->CanonicalPrograms.emplace(job.ProgramIdentity, interface);
    }
    return true;
}

StageFuture GetOrCompileStage(
    const shared_ptr<ShaderArtifactResolver::State>& state,
    const ProgramJob& job,
    render::ShaderStage stage,
    vector<string> projectedDefines,
    render::ShaderHash key) {
    std::scoped_lock lock{state->StageMutex};
    const auto existing = state->Stages.find(key);
    if (existing != state->Stages.end()) return existing->second;

    ShaderJitStageCompileRequest request{
        .ShaderRoot = state->ConfigValue.ShaderRoot,
        .Pass = job.Pass,
        .PassIndex = job.PassIndex,
        .Target = job.Target,
        .Stage = stage,
        .FullDefines = job.Defines,
        .ProjectedDefines = std::move(projectedDefines),
        .SourceIdentity = job.Source.Hash};
    StageFuture future = std::async(std::launch::async, [state, request = std::move(request)]() mutable {
                             ShaderJitStageCompileResult result;
                             {
                                 std::scoped_lock compilerLock{state->CompilerMutex};
                                 result = state->Compiler->CompileStage(request);
                             }
                             if (result.Succeeded() && ValidateCompiledStage(request, result)) {
                                 const auto identity = ComputeShaderSourceIdentity(request.ShaderRoot, request.Pass);
                                 if (!identity.Succeeded() || identity.Identity->Hash != request.SourceIdentity) {
                                     result.Artifact.reset();
                                     result.Diagnostics.emplace_back(render::ShaderDiagnostic{
                                         .Code = render::ShaderDiagnosticCode::SourceUnavailable,
                                         .Message = "shader source graph changed while JIT compilation was running",
                                         .Context = render::ShaderDiagnosticContext{
                                             .Target = request.Target,
                                             .PassIndex = request.PassIndex,
                                             .VariantDefines = request.FullDefines,
                                             .Stage = request.Stage}});
                                 }
                             }
                             return result;
                         }).share();
    state->Stages.emplace(key, future);
    return future;
}

ShaderArtifactResolutionResult CompileProgram(
    const shared_ptr<ShaderArtifactResolver::State>& state,
    ProgramJob job) {
    vector<StageFuture> futures;
    const render::ShaderHash toolchain = state->Compiler->GetToolchainHash();
    for (render::ShaderStage stage : GetPassStages(job.Pass)) {
        vector<string> projected = render::ProjectShaderDefines(job.Pass, stage, job.Defines);
        const render::ShaderHash key = BuildStageKey(job, stage, projected, toolchain);
        futures.emplace_back(GetOrCompileStage(state, job, stage, std::move(projected), key));
    }

    auto program = make_shared<ShaderResolvedProgram>();
    program->Target = job.Target;
    program->PassIndex = job.PassIndex;
    program->Defines = job.Defines;
    program->SourceIdentity = job.Source.Hash;
    program->ProgramIdentity = job.ProgramIdentity;
    ShaderArtifactResolutionResult result{
        .Program = {},
        .Diagnostics = {},
        .Source = ShaderArtifactSource::Jit};
    for (StageFuture& future : futures) {
        ShaderJitStageCompileResult stage = future.get();
        if (!stage.Succeeded()) {
            result.Diagnostics.insert(
                result.Diagnostics.end(),
                std::make_move_iterator(stage.Diagnostics.begin()),
                std::make_move_iterator(stage.Diagnostics.end()));
            return result;
        }
        program->Stages.emplace_back(std::move(*stage.Artifact));
    }

    render::ShaderInterfaceBuildResult interface;
    const render::ShaderDiagnosticContext programContext{
        .Target = job.Target,
        .PassIndex = job.PassIndex,
        .VariantDefines = job.Defines};
    if (std::holds_alternative<render::ShaderGraphicsPassDesc>(job.Pass.Program)) {
        interface = program->Stages.size() == 1
                        ? render::MergeGraphicsStageInterfaces(
                              program->Stages[0].Interface,
                              programContext)
                        : render::MergeGraphicsStageInterfaces(
                              program->Stages[0].Interface,
                              program->Stages[1].Interface,
                              programContext);
    } else {
        interface = render::BuildComputeShaderInterface(
            program->Stages[0].Interface,
            programContext);
    }
    if (!interface.Succeeded()) {
        result.Diagnostics = std::move(interface.Diagnostics);
        return result;
    }
    program->Interface = std::move(*interface.Interface);
    if (!ValidateCanonicalInterface(state, job, program->Interface, result.Diagnostics)) return result;
    result.Program = program;
    {
        std::scoped_lock diskLock{state->DiskMutex};
        (void)WriteDiskResult(state->ConfigValue, job, *program);
    }
    return result;
}

}  // namespace

render::ShaderHash ComputeShaderProgramIdentity(
    const render::ShaderPassDesc& pass,
    uint32_t passIndex,
    std::span<const string> fullDefines,
    render::ShaderHash sourceIdentity) noexcept {
    vector<string> normalized{fullDefines.begin(), fullDefines.end()};
    render::NormalizeShaderDefines(normalized);
    if (!render::IsShaderVariantInDomain(pass, normalized)) return {};
    BinaryWriter bytes;
    bytes.String("radray-shader-program-v1");
    WriteHash(bytes, sourceIdentity);
    bytes.U32(passIndex);
    WritePassCompileOptions(bytes, pass);
    bytes.Size32(normalized.size());
    for (const string& define : normalized) bytes.String(define);
    return render::HashShaderBytes(bytes.GetData());
}

ShaderArtifactResolver::ShaderArtifactResolver(
    AssetManager& assetManager,
    Config config,
    shared_ptr<IShaderJitCompiler> compiler) noexcept
    : _state(make_shared<State>()) {
    _state->Assets = &assetManager;
    _state->ConfigValue = std::move(config);
    _state->Compiler = std::move(compiler);
}

ShaderArtifactResolver::~ShaderArtifactResolver() noexcept = default;

ShaderArtifactResolver::Future ShaderArtifactResolver::ResolveAsync(
    const ShaderAsset& asset,
    render::ShaderTarget target,
    uint32_t passIndex,
    vector<string> fullDefines) {
    render::NormalizeShaderDefines(fullDefines);
    const render::ShaderBinary& binary = asset.GetBinary();
    if (!asset.IsValid() || !IsKnownTarget(target) || passIndex >= binary.Asset.Passes.size() ||
        !render::IsShaderVariantInDomain(binary.Asset.Passes[passIndex], fullDefines)) {
        ProgramJob invalidJob{
            .Pass = {},
            .PassIndex = passIndex,
            .Target = target,
            .Defines = std::move(fullDefines),
            .Source = {},
            .ExpectedInterface = {}};
        ShaderArtifactResolutionResult failure;
        failure.Diagnostics.emplace_back(MakeDiagnostic(
            render::ShaderDiagnosticCode::InvalidReflection,
            "shader artifact request is invalid or outside the variant domain",
            invalidJob));
        return MakeReadyFuture(std::move(failure));
    }
    if (auto baked = binary.FindProgramVariant(target, passIndex, fullDefines); baked.HasValue()) {
        return MakeReadyFuture(ResolveBaked(binary, *baked.Get(), passIndex));
    }
    if (_state->Compiler == nullptr || _state->Compiler->GetToolchainHash() == render::ShaderHash{}) {
        ProgramJob unavailableJob{
            .Pass = {},
            .PassIndex = passIndex,
            .Target = target,
            .Defines = std::move(fullDefines),
            .Source = {},
            .ExpectedInterface = {}};
        ShaderArtifactResolutionResult failure;
        failure.Diagnostics.emplace_back(MakeDiagnostic(
            render::ShaderDiagnosticCode::CompilationFailed,
            "runtime shader compiler is unavailable",
            unavailableJob));
        return MakeReadyFuture(std::move(failure));
    }

    const render::ShaderPassDesc& pass = binary.Asset.Passes[passIndex];
    std::optional<ShaderSourceIdentity> captured;
    ShaderSourceIdentityResult current;
    bool computedCurrent = false;
    bool sourceChanged = false;
    const ShaderAssetPassKey sourceKey{
        .Handle = asset.GetAssetHandle(),
        .PassIndex = passIndex};
    if (pass.SourceIdentity != render::ShaderHash{}) {
        captured = ShaderSourceIdentity{
            .Hash = pass.SourceIdentity,
            .Dependencies = {}};
    } else {
        if (_state->Assets == nullptr || !_state->Assets->IsHandleValid(sourceKey.Handle)) {
            ProgramJob sourceJob{
                .Pass = pass,
                .PassIndex = passIndex,
                .Target = target,
                .Defines = std::move(fullDefines),
                .Source = {},
                .ExpectedInterface = {}};
            ShaderArtifactResolutionResult failure;
            failure.Diagnostics.emplace_back(MakeDiagnostic(
                render::ShaderDiagnosticCode::SourceUnavailable,
                "a JIT-only ShaderAsset must be registered with the resolver's AssetManager",
                sourceJob));
            return MakeReadyFuture(std::move(failure));
        }
        std::scoped_lock sourceLock{_state->SourceMutex};
        PruneInvalidAssetSourcesLocked(*_state);
        const auto existing = _state->AssetSources.find(sourceKey);
        if (existing != _state->AssetSources.end()) captured = existing->second;
    }
    if (!captured.has_value()) {
        const bool hasBakedProgram = std::ranges::any_of(
            binary.ProgramVariants,
            [passIndex](const render::ShaderProgramVariantArtifact& program) {
                return program.PassIndex == passIndex;
            });
        if (hasBakedProgram) {
            ProgramJob sourceJob{
                .Pass = pass,
                .PassIndex = passIndex,
                .Target = target,
                .Defines = std::move(fullDefines),
                .Source = {},
                .ExpectedInterface = {}};
            ShaderArtifactResolutionResult failure;
            failure.Diagnostics.emplace_back(MakeDiagnostic(
                render::ShaderDiagnosticCode::SourceUnavailable,
                "a partially baked pass requires a cooked source identity before JIT compilation",
                sourceJob));
            return MakeReadyFuture(std::move(failure));
        }
        current = ComputeShaderSourceIdentity(_state->ConfigValue.ShaderRoot, pass);
        computedCurrent = true;
        if (!current.Succeeded()) {
            ProgramJob sourceJob{
                .Pass = pass,
                .PassIndex = passIndex,
                .Target = target,
                .Defines = std::move(fullDefines),
                .Source = {},
                .ExpectedInterface = {}};
            ShaderArtifactResolutionResult failure;
            failure.Diagnostics.emplace_back(MakeDiagnostic(
                render::ShaderDiagnosticCode::SourceUnavailable,
                std::move(current.Error),
                sourceJob));
            return MakeReadyFuture(std::move(failure));
        }
        std::scoped_lock sourceLock{_state->SourceMutex};
        PruneInvalidAssetSourcesLocked(*_state);
        const auto [it, inserted] = _state->AssetSources.emplace(sourceKey, *current.Identity);
        captured = it->second;
        sourceChanged = !inserted && captured->Hash != current.Identity->Hash;
    }

    ProgramJob job{
        .Pass = pass,
        .PassIndex = passIndex,
        .Target = target,
        .Defines = std::move(fullDefines),
        .Source = *captured,
        .ExpectedInterface = {}};
    job.ProgramIdentity = ComputeShaderProgramIdentity(
        job.Pass,
        job.PassIndex,
        job.Defines,
        job.Source.Hash);
    if (job.ProgramIdentity == render::ShaderHash{}) {
        ShaderArtifactResolutionResult failure;
        failure.Diagnostics.emplace_back(MakeDiagnostic(
            render::ShaderDiagnosticCode::InvalidReflection,
            "failed to compute the immutable shader program identity",
            job));
        return MakeReadyFuture(std::move(failure));
    }
    for (const render::ShaderProgramVariantArtifact& candidate : binary.ProgramVariants) {
        if (candidate.PassIndex == passIndex && candidate.Defines == job.Defines) {
            job.ExpectedInterface = binary.ProgramInterfaces[candidate.InterfaceIndex];
            break;
        }
    }
    const render::ShaderHash toolchain = _state->Compiler->GetToolchainHash();
    job.ProgramKey = BuildProgramKey(job, toolchain);

    {
        std::scoped_lock programLock{_state->ProgramMutex};
        const auto existing = _state->Programs.find(job.ProgramKey);
        if (existing != _state->Programs.end()) return existing->second;
    }
    {
        std::scoped_lock diskLock{_state->DiskMutex};
        auto disk = ReadDiskResult(_state->ConfigValue, job);
        if (disk.has_value()) {
            if (!ValidateCanonicalInterface(_state, job, disk->Program->Interface, disk->Diagnostics)) {
                return MakeReadyFuture(std::move(*disk));
            }
            ShaderFuture ready = MakeReadyFuture(std::move(*disk));
            std::scoped_lock programLock{_state->ProgramMutex};
            const auto [it, inserted] = _state->Programs.emplace(job.ProgramKey, ready);
            return inserted ? ready : it->second;
        }
    }
    if (!computedCurrent) {
        current = ComputeShaderSourceIdentity(_state->ConfigValue.ShaderRoot, pass);
        computedCurrent = true;
    }
    if (!current.Succeeded()) {
        ShaderArtifactResolutionResult failure;
        failure.Diagnostics.emplace_back(MakeDiagnostic(
            render::ShaderDiagnosticCode::SourceUnavailable,
            std::move(current.Error),
            job));
        ShaderFuture ready = MakeReadyFuture(std::move(failure));
        std::scoped_lock programLock{_state->ProgramMutex};
        const auto [it, inserted] = _state->Programs.emplace(job.ProgramKey, ready);
        return inserted ? ready : it->second;
    }
    sourceChanged = sourceChanged || captured->Hash != current.Identity->Hash;
    if (sourceChanged) {
        ShaderArtifactResolutionResult failure;
        failure.Diagnostics.emplace_back(MakeDiagnostic(
            render::ShaderDiagnosticCode::SourceUnavailable,
            "shader source changed; create a new immutable ShaderAsset before compiling new artifacts",
            job));
        ShaderFuture ready = MakeReadyFuture(std::move(failure));
        std::scoped_lock programLock{_state->ProgramMutex};
        const auto [it, inserted] = _state->Programs.emplace(job.ProgramKey, ready);
        return inserted ? ready : it->second;
    }

    std::scoped_lock programLock{_state->ProgramMutex};
    const auto existing = _state->Programs.find(job.ProgramKey);
    if (existing != _state->Programs.end()) return existing->second;
    const render::ShaderHash programKey = job.ProgramKey;
    ShaderFuture future = std::async(std::launch::async, [state = _state, job = std::move(job)]() mutable {
                              return CompileProgram(state, std::move(job));
                          }).share();
    _state->Programs.emplace(programKey, future);
    return future;
}

void ShaderArtifactResolver::ClearMemoryCache() noexcept {
    if (_state == nullptr) return;
    std::scoped_lock programLock{_state->ProgramMutex};
    std::scoped_lock stageLock{_state->StageMutex};
    std::scoped_lock canonicalLock{_state->CanonicalMutex};
    std::scoped_lock sourceLock{_state->SourceMutex};
    _state->Programs.clear();
    _state->Stages.clear();
    _state->CanonicalPrograms.clear();
    PruneInvalidAssetSourcesLocked(*_state);
}

size_t ShaderArtifactResolver::GetProgramCacheSize() const noexcept {
    if (_state == nullptr) return 0;
    std::scoped_lock lock{_state->ProgramMutex};
    return _state->Programs.size();
}

size_t ShaderArtifactResolver::GetStageCacheSize() const noexcept {
    if (_state == nullptr) return 0;
    std::scoped_lock lock{_state->StageMutex};
    return _state->Stages.size();
}

size_t ShaderArtifactResolver::GetCapturedSourceIdentityCount() const noexcept {
    if (_state == nullptr) return 0;
    std::scoped_lock lock{_state->SourceMutex};
    return _state->AssetSources.size();
}

#if defined(RADRAY_ENABLE_SHADER_JIT)
namespace {

class DxcShaderJitCompiler final : public IShaderJitCompiler {
public:
    explicit DxcShaderJitCompiler(shared_ptr<render::Dxc> dxc) noexcept
        : _dxc(std::move(dxc)) {}

    render::ShaderHash GetToolchainHash() const noexcept override {
        if (_dxc == nullptr) return {};
        BinaryWriter identity;
        identity.String("radray-dxc-jit-v1");
        WriteHash(identity, _dxc->GetToolchainHash());
        return render::HashShaderBytes(identity.GetData());
    }

    ShaderJitStageCompileResult CompileStage(
        const ShaderJitStageCompileRequest& request) override {
        ShaderJitStageCompileResult result;
        render::ShaderDiagnosticContext context{
            .Target = request.Target,
            .PassIndex = request.PassIndex,
            .VariantDefines = request.FullDefines,
            .Stage = request.Stage};
        if (_dxc == nullptr || !_dxc->IsValid()) {
            result.Diagnostics.emplace_back(render::ShaderDiagnostic{
                .Code = render::ShaderDiagnosticCode::CompilationFailed,
                .Message = "DXC is unavailable",
                .Context = context});
            return result;
        }
        const auto entry = render::FindShaderEntryPoint(request.Pass, request.Stage);
        if (!entry.has_value()) {
            result.Diagnostics.emplace_back(render::ShaderDiagnostic{
                .Code = render::ShaderDiagnosticCode::CompilationFailed,
                .Message = "requested shader stage has no entry point",
                .Context = context});
            return result;
        }
        vector<string> includeStrings{request.ShaderRoot.string()};
        for (const string& includeDir : request.Pass.IncludeDirs) {
            includeStrings.emplace_back(
                (request.ShaderRoot /
                 std::filesystem::path{std::u8string{includeDir.begin(), includeDir.end()}})
                    .string());
        }
        vector<std::string_view> includes;
        for (const string& include : includeStrings) includes.emplace_back(include);
        vector<std::string_view> defines;
        for (const string& define : request.ProjectedDefines) defines.emplace_back(define);
        render::DxcCompileOptions options{
            .EntryPoint = *entry,
            .Stage = request.Stage,
            .SM = request.Pass.SM,
            .Defines = defines,
            .Includes = includes,
            .IsOptimize = request.Pass.IsOptimize,
            .IsSpirv = request.Target == render::ShaderTarget::SPIRV,
            .EnableUnbounded = request.Pass.EnableUnbounded};
        auto compiled = _dxc->CompileFile(
            request.ShaderRoot /
                std::filesystem::path{std::u8string{
                    request.Pass.SourcePath.begin(), request.Pass.SourcePath.end()}},
            options);
        if (!compiled.has_value()) {
            result.Diagnostics.emplace_back(render::ShaderDiagnostic{
                .Code = render::ShaderDiagnosticCode::CompilationFailed,
                .Message = fmt::format("DXC failed to compile entry '{}'", *entry),
                .Context = context});
            return result;
        }

        render::ShaderReflectionDesc reflection;
        if (request.Target == render::ShaderTarget::DXIL) {
            auto value = _dxc->GetShaderDescFromOutput(compiled->Refl);
            if (!value.has_value()) {
                result.Diagnostics.emplace_back(render::ShaderDiagnostic{
                    .Code = render::ShaderDiagnosticCode::CompilationFailed,
                    .Message = "DXIL reflection failed",
                    .Context = context});
                return result;
            }
            reflection = std::move(*value);
        } else {
#if defined(RADRAY_ENABLE_SPIRV_CROSS)
            auto value = render::ReflectSpirv(render::SpirvBytecodeView{
                .Data = compiled->Data,
                .EntryPointName = *entry,
                .Stage = request.Stage});
            if (!value.has_value()) {
                result.Diagnostics.emplace_back(render::ShaderDiagnostic{
                    .Code = render::ShaderDiagnosticCode::CompilationFailed,
                    .Message = "SPIR-V reflection failed",
                    .Context = context});
                return result;
            }
            reflection = std::move(*value);
#else
            result.Diagnostics.emplace_back(render::ShaderDiagnostic{
                .Code = render::ShaderDiagnosticCode::CompilationFailed,
                .Message = "SPIR-V JIT requires SPIRV-Cross",
                .Context = context});
            return result;
#endif
        }
        render::ShaderInterfaceNormalizationOptions normalization{
            .Context = context,
            .PushConstantBindings = {}};
        render::ShaderStageInterfaceBuildResult interface;
        if (const auto* hlsl = std::get_if<render::HlslShaderDesc>(&reflection)) {
            interface = render::NormalizeHlslInterface(*hlsl, request.Stage, normalization);
        } else {
            interface = render::NormalizeSpirvInterface(
                std::get<render::SpirvShaderDesc>(reflection), request.Stage, normalization);
        }
        if (!interface.Succeeded()) {
            result.Diagnostics = std::move(interface.Diagnostics);
            return result;
        }
        ShaderResolvedStageArtifact artifact{
            .Target = request.Target,
            .PassIndex = request.PassIndex,
            .Stage = request.Stage,
            .Defines = request.ProjectedDefines,
            .EntryPoint = string{*entry},
            .Bytecode = std::move(compiled->Data),
            .Reflection = std::move(reflection),
            .Interface = std::move(*interface.Interface)};
        artifact.BinaryHash = render::HashShaderBytes(artifact.Bytecode);
        result.Artifact = std::move(artifact);
        return result;
    }

private:
    shared_ptr<render::Dxc> _dxc;
};

}  // namespace

shared_ptr<IShaderJitCompiler> CreateDxcShaderJitCompiler(
    shared_ptr<render::Dxc> dxc) noexcept {
    if (dxc == nullptr || !dxc->IsValid()) return nullptr;
    return make_shared<DxcShaderJitCompiler>(std::move(dxc));
}
#endif

}  // namespace radray
