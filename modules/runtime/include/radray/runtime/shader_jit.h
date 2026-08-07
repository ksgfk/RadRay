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
