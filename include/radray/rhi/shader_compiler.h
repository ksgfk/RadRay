#pragma once

#include <string>
#include <vector>
#include <variant>
#include <string_view>
#include <span>

struct RadrayCompileRasterizationShaderDescriptor;

namespace radray::rhi::shader_compiler {

struct DxilShaderBlob {
    std::vector<uint8_t> Data;
    std::vector<uint8_t> Reflection;
};

using CompileResult = std::variant<DxilShaderBlob, std::string>;

class DxcShaderCompiler {
public:
    DxcShaderCompiler();
    ~DxcShaderCompiler() noexcept;

    CompileResult Compile(std::string_view code, std::span<std::string_view> args) const;
    CompileResult Compile(const RadrayCompileRasterizationShaderDescriptor* desc) const;

private:
    class Impl;

    Impl* _impl;
};

}  // namespace radray::rhi
