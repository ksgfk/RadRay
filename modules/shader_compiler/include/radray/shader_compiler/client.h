#pragma once

#include <filesystem>
#include <span>
#include <string_view>

#include <radray/dynamic_library.h>
#include <radray/shader_compiler/contract_discovery.h>

namespace radray::shader_compiler {

class Client {
public:
    explicit Client(std::string_view compilerLibraryName = "dxcompiler") noexcept;

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    bool IsAvailable() const noexcept;

    DiscoveryResult DiscoverSourceContract(
        std::string_view sourceName,
        std::span<const byte> source,
        shader::ShaderTarget target,
        std::span<const std::filesystem::path> includePaths) const;

    shader::CompileVariantResult CompileVariant(
        const shader::CompileVariantRequest& request,
        std::span<const std::filesystem::path> includePaths) const;

private:
    DynamicLibrary _compilerLibrary;
};

}  // namespace radray::shader_compiler
