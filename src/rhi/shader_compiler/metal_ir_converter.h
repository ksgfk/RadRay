#pragma once

#ifdef RADRAYSC_ENABLE_MSC

#include <array>

#include "shader_compiler_impl.h"

class IRCompiler;

class ShaderCompilerImpl::MscImpl {
public:
    MscImpl(ShaderCompilerImpl* sc) noexcept;
    ~MscImpl() noexcept;

private:
    ShaderCompilerImpl* _sc;
    void* _mscLib{nullptr};
    std::array<void*, 12> _vtbl;
    IRCompiler* _compiler{nullptr};
};

#endif
