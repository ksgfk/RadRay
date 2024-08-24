#pragma once

#include <variant>
#include <string_view>
#include <span>

#include <radray/types.h>

class IDxcCompiler3;
class IDxcUtils;

namespace radray::rhi {

struct ShaderBlob {
    radray::vector<uint8_t> Data;
    radray::vector<uint8_t> Reflection;
};

using CompileResult = std::variant<ShaderBlob, radray::string>;

class DxcShaderCompiler {
public:
    DxcShaderCompiler();
    ~DxcShaderCompiler() noexcept;

    CompileResult Compile(std::string_view code, std::span<const wchar_t*> args) const;

    IDxcCompiler3* GetCompiler() const noexcept;
    IDxcUtils* GetUtils() const noexcept;

private:
    class Impl;

    Impl* _impl;
};

}  // namespace radray::rhi
