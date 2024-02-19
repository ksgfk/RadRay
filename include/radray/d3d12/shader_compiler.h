#pragma once

#include <radray/d3d12/utility.h>
#include <dxcapi.h>

namespace radray::d3d12 {

class ShaderCompiler {
public:
    ShaderCompiler() noexcept;

    ComPtr<IDxcCompiler3> compiler;
};

}  // namespace radray::d3d12
