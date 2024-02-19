#include <radray/d3d12/shader_compiler.h>

namespace radray::d3d12 {

ShaderCompiler::ShaderCompiler() noexcept {
    ThrowIfFailed(DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(compiler.GetAddressOf())));
}

}  // namespace radray::d3d12
