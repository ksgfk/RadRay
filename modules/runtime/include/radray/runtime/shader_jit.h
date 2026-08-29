#pragma once

#include <filesystem>
#include <optional>
#include <string_view>

#include <radray/shader/shader_artifact.h>
#include <radray/shader/shader_compiler_contract.h>

namespace radray {

struct ShaderJitArtifact {
    shader::ShaderTarget Target{shader::ShaderTarget::DXIL};
    vector<byte> Metadata;
    shader::GpuArtifactHash ExpectedGpuArtifact{};
};

class ShaderJit {
public:
    explicit ShaderJit(
        vector<std::filesystem::path> includePaths,
        std::string_view compilerLibraryName = "dxcompiler") noexcept;
    ~ShaderJit() noexcept;

    bool IsAvailable() const noexcept;

    // Identity of the loaded compiler toolchain. Empty when no compiler is available.
    std::optional<shader::Hash128> GetToolchainIdentity() const;

    // Discovery has to see the same source, defines and policy the compile will use: a contract
    // discovered under a different policy can describe a different set of entry points.
    std::optional<shader::ContractHash> DiscoverContractHash(
        const shader::SourceContractRequest& request) const;

    // Convenience for callers with nothing but a source: default policy, no defines.
    std::optional<shader::ContractHash> DiscoverContractHash(
        std::string_view sourceName,
        std::span<const byte> source,
        shader::ShaderTarget target) const;

    std::optional<ShaderJitArtifact> Compile(
        const shader::CompileVariantRequest& request,
        shader::ShaderTarget target) const;

private:
    class ClientHolder;
    unique_ptr<ClientHolder> _client;
};

}  // namespace radray
