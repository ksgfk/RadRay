#pragma once

#include "shader_compiler_impl.h"

class IDxcCompiler3;
class IDxcUtils;
class IDxcIncludeHandler;

class ShaderCompilerImpl::DxcImpl {
public:
    DxcImpl(ShaderCompilerImpl* sc) noexcept;
    ~DxcImpl() noexcept;

    CompileResultDxil DxcCompileHlsl(std::string_view code, std::span<std::string_view> args) const noexcept;

    CreateReflectResult DxcCreateReflection(std::span<const uint8_t> refl) const noexcept;

private:
    ShaderCompilerImpl* _sc;
    IDxcCompiler3* _dxc{nullptr};
    IDxcUtils* _dxcUtil{nullptr};
    IDxcIncludeHandler* _dxcInc{nullptr};
};
