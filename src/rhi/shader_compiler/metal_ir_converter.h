#pragma once

#ifdef RADRAYSC_ENABLE_MSC

#include <array>

#include "shader_compiler_impl.h"

class IRCompiler;

class ShaderCompilerImpl::MscImpl {
public:
    MscImpl(ShaderCompilerImpl* sc) noexcept;
    ~MscImpl() noexcept;

    ConvertResultMetallib DxilToMetallib(std::span<const uint8_t> dxil, RadrayShaderCompilerMetalStage stage) const noexcept;

private:
    ShaderCompilerImpl* _sc;
    IRCompiler* _ir{nullptr};
};

#endif
