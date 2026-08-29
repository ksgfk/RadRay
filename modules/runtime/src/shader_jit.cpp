#include <radray/runtime/shader_jit.h>

#if defined(RADRAY_ENABLE_SHADER_JIT)

#include <radray/shader_compiler/client.h>
#include <radray/logger.h>

#include <cstring>

namespace radray {

class ShaderJit::ClientHolder {
public:
    explicit ClientHolder(
        vector<std::filesystem::path> includePaths,
        std::string_view compilerLibraryName) noexcept
        : IncludePaths(std::move(includePaths)), Client(compilerLibraryName) {}

    vector<std::filesystem::path> IncludePaths;
    shader_compiler::Client Client;
};

ShaderJit::ShaderJit(
    vector<std::filesystem::path> includePaths,
    std::string_view compilerLibraryName) noexcept
    : _client(make_unique<ClientHolder>(std::move(includePaths), compilerLibraryName)) {}

ShaderJit::~ShaderJit() noexcept = default;

bool ShaderJit::IsAvailable() const noexcept {
    return _client != nullptr && _client->Client.IsAvailable();
}

std::optional<shader::Hash128> ShaderJit::GetToolchainIdentity() const {
    if (!IsAvailable()) {
        return std::nullopt;
    }
    return _client->Client.GetToolchainIdentity();
}

std::optional<shader::ContractHash> ShaderJit::DiscoverContractHash(
    std::string_view sourceName,
    std::span<const byte> source,
    shader::ShaderTarget target) const {
    shader::SourceContractRequest request;
    request.SourceName = string{sourceName};
    request.RootSource.assign(source.begin(), source.end());
    request.Targets = static_cast<shader::ShaderTargetMask>(shader::ToTargetMask(target));
    return DiscoverContractHash(request);
}

std::optional<shader::ContractHash> ShaderJit::DiscoverContractHash(
    const shader::SourceContractRequest& request) const {
    if (!IsAvailable()) {
        return std::nullopt;
    }
    const shader_compiler::DiscoveryResult result =
        _client->Client.DiscoverSourceContract(request, _client->IncludePaths);
    if (!result.Succeeded()) {
        return std::nullopt;
    }
    return result.Contract.Hash;
}

std::optional<ShaderJitArtifact> ShaderJit::Compile(
    const shader::CompileVariantRequest& request,
    shader::ShaderTarget target) const {
    if (!IsAvailable() || !shader::HasTarget(request.Targets, target)) {
        return std::nullopt;
    }
    shader::CompileVariantRequest concreteRequest = request;
    concreteRequest.Targets = static_cast<shader::ShaderTargetMask>(shader::ToTargetMask(target));
    const shader::CompileVariantResult result =
        _client->Client.CompileVariant(concreteRequest, _client->IncludePaths);
    if (result.Status != shader::CompileStatus::Success || result.Lanes.size() != 1) {
        RADRAY_ERR_LOG("ShaderJit error:");
        for (const auto& i : result.Diagnostics) {
            RADRAY_ERR_LOG("  {}, {}", i.Code, i.Message);
        }
        return std::nullopt;
    }
    const shader::CompileTargetLane& lane = result.Lanes.front();
    if (lane.Target != target || lane.Metadata.size() < sizeof(shader::WireMetadataEnvelope)) {
        return std::nullopt;
    }
    shader::WireMetadataEnvelope envelope{};
    std::memcpy(&envelope, lane.Metadata.data(), sizeof(envelope));
    if (!shader::ValidateWireMetadataEnvelope(lane.Metadata, target, envelope.GpuArtifact)) {
        return std::nullopt;
    }
    return ShaderJitArtifact{
        .Target = target,
        .Metadata = lane.Metadata,
        .ExpectedGpuArtifact = envelope.GpuArtifact};
}

}  // namespace radray

#else

namespace radray {

class ShaderJit::ClientHolder {};

ShaderJit::ShaderJit(vector<std::filesystem::path>, std::string_view) noexcept {}

ShaderJit::~ShaderJit() noexcept = default;

bool ShaderJit::IsAvailable() const noexcept {
    return false;
}

std::optional<shader::Hash128> ShaderJit::GetToolchainIdentity() const {
    return std::nullopt;
}

std::optional<shader::ContractHash> ShaderJit::DiscoverContractHash(
    std::string_view,
    std::span<const byte>,
    shader::ShaderTarget) const {
    return std::nullopt;
}

std::optional<shader::ContractHash> ShaderJit::DiscoverContractHash(
    const shader::SourceContractRequest&) const {
    return std::nullopt;
}

std::optional<ShaderJitArtifact> ShaderJit::Compile(
    const shader::CompileVariantRequest&,
    shader::ShaderTarget) const {
    return std::nullopt;
}

}  // namespace radray

#endif
