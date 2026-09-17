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
size_t ShaderProgramCache::GetLayoutPreparationCount() const noexcept {
    std::lock_guard lock(_mutex);
    return _layoutPreparationCount;
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

// In-memory key only. Scalar fields and the padding-free wire sampler record are encoded exactly;
// invalid duplicate selectors cannot alias a previously validated request.
template <typename T>
void AppendRecipeValue(vector<byte>& key, const T& value) {
    static_assert(std::is_trivially_copyable_v<T>);
    const auto bytes = std::as_bytes(std::span{&value, 1});
    key.insert(key.end(), bytes.begin(), bytes.end());
}

template <typename Modifier, typename AppendPayload>
bool AppendRecipeModifiers(vector<byte>& key, const vector<Modifier>& modifiers, AppendPayload appendPayload) {
    vector<const Modifier*> sorted;
    sorted.reserve(modifiers.size());
    for (const auto& modifier : modifiers) sorted.push_back(&modifier);
    std::sort(sorted.begin(), sorted.end(), [](const auto* lhs, const auto* rhs) {
        return lhs->Selector < rhs->Selector;
    });
    AppendRecipeValue(key, sorted.size());
    for (size_t index = 0; index < sorted.size(); ++index) {
        const auto& modifier = *sorted[index];
        const auto& selector = modifier.Selector;
        if (index != 0 && sorted[index - 1]->Selector.DeclarationName == selector.DeclarationName) {
            RADRAY_ERR_LOG("shader layout recipe has duplicate selector '{}'", selector.DeclarationName);
            return false;
        }
        AppendRecipeValue(key, selector.DeclarationName.size());
        const auto name = std::as_bytes(std::span{selector.DeclarationName.data(), selector.DeclarationName.size()});
        key.insert(key.end(), name.begin(), name.end());
        AppendRecipeValue(key, selector.ExpectedLogicalResourceKind);
        appendPayload(key, modifier);
    }
    return true;
}

std::optional<vector<byte>> MakeRecipeKey(render::RenderBackend backend, const render::ShaderProgramLayoutRecipe& recipe) {
    vector<byte> key;
    const auto placement = [](auto& bytes, const auto& modifier) { AppendRecipeValue(bytes, modifier.Placement); };
    switch (backend) {
        case render::RenderBackend::D3D12:
            if (!AppendRecipeModifiers(key, recipe.D3D12.BufferPlacements, placement)) return std::nullopt;
            break;
        case render::RenderBackend::Vulkan:
            if (!AppendRecipeModifiers(key, recipe.Vulkan.BufferDescriptors, placement) ||
                !AppendRecipeModifiers(key, recipe.Vulkan.ImmutableSamplers, [](auto& bytes, const auto& modifier) {
                    static_assert(sizeof(shader::WireSamplerRecord) == 64);
                    AppendRecipeValue(bytes, modifier.State);
                })) return std::nullopt;
            break;
        default: return std::nullopt;
    }
    return key;
}

}  // namespace

size_t ShaderProgramCache::RecipeKeyHash::operator()(const RecipeKey& value) const noexcept {
    HashCode hash;
    for (const auto byteValue : value) hash.Add(static_cast<uint8_t>(byteValue));
    return hash.ToHashCode();
}

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
Nullable<ShaderProgramCache::ArtifactRecord*> ShaderProgramCache::GetOrCompileArtifact(
    const ShaderProgramRequest& request,
    ArtifactKey key) {
    const auto cached = _shaderArtifacts.find(key);
    if (cached != _shaderArtifacts.end()) {
        return cached->second.Failed ? nullptr : &cached->second;
    }

    const auto fail = [&]() -> Nullable<ArtifactRecord*> {
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

    const Nullable<ArtifactRecord*> artifactRecord =
        GetOrCompileArtifact(request, std::move(artifactKey));
    if (!artifactRecord.HasValue()) {
        return nullptr;
    }
    const ShaderJitArtifact& compiled = artifactRecord.Get()->Artifact;
    const shader::ShaderArtifactDecodeOptions decodeOptions{
        .Target = compiled.Target,
        .ExpectedGpuArtifact = compiled.ExpectedGpuArtifact};

    auto recipeKey = MakeRecipeKey(_device.GetBackend(), request.LayoutRecipe);
    if (!recipeKey) return nullptr;
    auto& recipes = artifactRecord->ResolvedRecipes;
    if (const auto found = recipes.find(*recipeKey); found != recipes.end()) {
        const auto cached = _shaderPrograms.find(ProgramKey{artifactRecord->Identity, found->second});
        RADRAY_ASSERT(cached != _shaderPrograms.end());
        return cached->second.Failed ? nullptr : cached->second.Program.get();
    }

    render::BackendShaderArtifactError artifactError;
    ++_layoutPreparationCount;
    auto prepared = render::PrepareBackendShaderArtifact(
        _device.GetBackend(), compiled.Metadata, decodeOptions, request.LayoutRecipe, &artifactError);
    if (!prepared) {
        RADRAY_ERR_LOG("shader program '{}' layout resolve failed: {}:{}", request.SourceName,
                       static_cast<uint32_t>(artifactError.Failure), static_cast<uint32_t>(artifactError.DecodeFailure));
        return nullptr;
    }
    const ProgramKey programKey{artifactRecord->Identity, prepared->LayoutHash()};
    recipes.emplace(std::move(*recipeKey), programKey.LayoutHash);
    const auto cached = _shaderPrograms.find(programKey);
    if (cached != _shaderPrograms.end()) {
        return cached->second.Failed ? nullptr : cached->second.Program.get();
    }

    auto artifact = render::CreateBackendShaderArtifact(_device, std::move(*prepared), &artifactError);
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
    auto recipeKey = MakeRecipeKey(device.GetBackend(), recipe);
    if (!recipeKey) return nullptr;
    // Caller memory may change in place. Only identical bytes, expected identity and recipe may
    // reuse prior validation; pointer identity or the caller-supplied digest alone is insufficient.
    for (auto& value : _precompiledPrograms) {
        if (value.ExpectedIdentity == expectedIdentity && std::ranges::equal(value.Bytes, bytes) &&
            std::ranges::find(value.Recipes, *recipeKey) != value.Recipes.end()) return value.Program.get();
    }
    render::BackendShaderArtifactError error;
    ++_layoutPreparationCount;
    auto prepared = render::PrepareBackendShaderArtifact(device.GetBackend(), bytes, options, recipe, &error);
    if (!prepared) {
        RADRAY_ERR_LOG("Precompiled shader layout failed: {}:{}", uint32_t(error.Failure), uint32_t(error.DecodeFailure));
        return nullptr;
    }
    for (auto& value : _precompiledPrograms) {
        if (value.ExpectedIdentity == expectedIdentity && value.LayoutHash == prepared->LayoutHash() &&
            std::ranges::equal(value.Bytes, bytes)) {
            value.Recipes.push_back(std::move(*recipeKey));
            return value.Program.get();
        }
    }
    PrecompiledProgram value;
    value.Bytes.assign(bytes.begin(), bytes.end());
    value.LayoutHash = prepared->LayoutHash();
    value.ExpectedIdentity = expectedIdentity;
    value.Recipes.push_back(std::move(*recipeKey));
    auto artifact = render::CreateBackendShaderArtifact(device, std::move(*prepared), &error);
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
