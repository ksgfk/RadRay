#pragma once

#include <span>
#include <string_view>

#include <radray/shader/shader_compiler_contract.h>

namespace radray::shader_compiler {

using DiscoveryResult = shader::DiagnosticResult<shader::ShaderContract>;

DiscoveryResult DecodeWireShaderContract(std::span<const byte> blob);

}  // namespace radray::shader_compiler
