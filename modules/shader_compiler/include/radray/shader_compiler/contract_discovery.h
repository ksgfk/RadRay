#pragma once

#include <span>
#include <string_view>

#include <radray/shader/shader_compiler_contract.h>

namespace radray::shader_compiler {

struct DiscoveryResult {
    shader::CompileStatus Status{shader::CompileStatus::InvalidRequest};
    shader::ShaderContract Contract{};
    vector<shader::CompileDiagnostic> Diagnostics;

    bool Succeeded() const noexcept { return Status == shader::CompileStatus::Success; }
};

DiscoveryResult DecodeWireShaderContract(std::span<const byte> blob);

}  // namespace radray::shader_compiler
