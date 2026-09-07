#include "shader_program_cache.h"

#include <algorithm>
#include <cstring>
#include <type_traits>
#include <radray/file.h>
#include <radray/hash.h>
#include <radray/logger.h>
#include <radray/render/backend_shader_artifact.h>

namespace radray {

ShaderProgramCache::ShaderProgramCache(render::Device& device, std::filesystem::path sourceRoot, vector<std::filesystem::path> includePaths)
    : _device(device), _sourceRoot(std::move(sourceRoot)), _shaderJit(make_unique<ShaderJit>(std::move(includePaths))) {}
ShaderProgramCache::~ShaderProgramCache() noexcept = default;
size_t ShaderProgramCache::GetProgramCount() const noexcept {
    std::lock_guard lock(_mutex);
    return _shaderPrograms.size() + _precompiledPrograms.size();
}
size_t ShaderProgramCache::GetArtifactCount() const noexcept {
    std::lock_guard lock(_mutex);
    return _shaderArtifacts.size();
}
bool ShaderProgramCache::InvalidateSource(std::string_view sourceName) {
    if (!shader::IsLogicalSourceName(sourceName)) return false;
    std::lock_guard lock(_mutex);
    auto& revision = _sourceRevisions[string{sourceName}];
    if (revision == UINT64_MAX) RADRAY_ABORT("Shader source revision exhausted");
    ++revision;
    return true;
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

size_t ShaderProgramCache::ArtifactKeyHash::operator()(const ArtifactKey& value) const noexcept {
    HashCode hash;
    hash.Add(value.SourceName);
    hash.Add(value.SourceRevision);
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

size_t ShaderProgramCache::ProgramKeyHash::operator()(const ProgramKey& value) const noexcept {
    HashCode hash;
    hash.Add(value.ArtifactIdentity);
    AddValueBytes(hash, value.LayoutHash.Bytes);
    return hash.ToHashCode();
}

// Compiles once per artifact key and remembers the outcome, success or failure, under that key. The
// returned record is owned by the cache; it stays valid until the cache is cleared.
Nullable<const ShaderProgramCache::ArtifactRecord*> ShaderProgramCache::GetOrCompileArtifact(
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
        _sourceRoot.empty()) {
        RADRAY_ERR_LOG(
            "shader program '{}' unavailable: invalid source name or empty source root",
            request.SourceName);
        return fail();
    }
    const std::filesystem::path sourcePath =
        _sourceRoot / std::filesystem::path{request.SourceName};
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

Nullable<ShaderProgram*> ShaderProgramCache::GetOrCreateShaderProgram(
    const ShaderProgramRequest& request) {
    std::lock_guard lock(_mutex);
    if (_shaderJit == nullptr ||
        !_shaderJit->IsAvailable()) {
        RADRAY_ERR_LOG(
            "shader program '{}' unavailable: shader JIT is disabled or unavailable",
            request.SourceName);
        return nullptr;
    }
    const std::optional<shader::ShaderTarget> target =
        render::GetShaderTargetForBackend(_device.GetBackend());
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
        .SourceRevision = _sourceRevisions[request.SourceName],
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
            _device.GetBackend(),
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
            _device,
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
        ShaderProgram::Create(&_device, std::move(artifact.value()));
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

Nullable<ShaderProgram*> ShaderProgramCache::GetOrCreateShaderProgram(
    std::span<const byte> bytes, const shader::GpuArtifactHash& expectedIdentity, const render::ShaderProgramLayoutRecipe& recipe) {
    std::lock_guard lock(_mutex);
    if (bytes.empty()) return nullptr;
    auto& device = _device;
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

}  // namespace radray
