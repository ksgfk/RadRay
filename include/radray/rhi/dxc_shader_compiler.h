#pragma once

#include <variant>
#include <string_view>
#include <span>

#include <radray/types.h>

namespace radray::rhi {

using CompileResult = std::variant<radray::vector<uint8_t>, radray::string>;

class DxcShaderCompiler {
public:
    DxcShaderCompiler();

    CompileResult Compile(std::string_view code, std::span<const wchar_t*> args) const;

private:
    class Impl;

    radray::unique_ptr<Impl> _impl;
};

}  // namespace radray::rhi
