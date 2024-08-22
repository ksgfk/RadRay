#pragma once

#include <variant>
#include <string_view>
#include <span>

#include <radray/rhi/config.h>

namespace radray::rhi {

using CompileResult = std::variant<RhiVector<uint8_t>, std::string>;

class DxcShaderCompiler {
public:
    DxcShaderCompiler();

    CompileResult Compile(std::string_view code, std::span<const wchar_t*> args) const;

private:
    class Impl;

    RhiUniquePtr<Impl> _impl;
};

}  // namespace radray::rhi
