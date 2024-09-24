#pragma once

#include <radray/platform.h>
#include <radray/utility.h>
#include <radray/rhi/shader_compiler.h>

namespace radray::rhi {

class ShaderCompilerBridge;

// class ShaderBlob {
// private:
//     ShaderCompiler* _sc;
//     RadrayCompilerBlob _blob;
// };

class ShaderCompilerBridge {
public:
    ShaderCompilerBridge();
    ~ShaderCompilerBridge() noexcept;
    RADRAY_NO_COPY_CTOR(ShaderCompilerBridge);
    ShaderCompilerBridge(ShaderCompilerBridge&& other) noexcept;
    ShaderCompilerBridge& operator=(ShaderCompilerBridge&& other) noexcept;

    bool IsAvailable(RadrayShaderCompilerType type) const noexcept;

private:
    DynamicLibrary _scLib;
    RadrayShaderCompiler* _shaderCompiler;

    std::add_pointer_t<decltype(RadrayCreateShaderCompiler)> CreateShaderCompiler;
    std::add_pointer_t<decltype(RadrayReleaseShaderCompiler)> ReleaseShaderCompiler;
};

}  // namespace radray::rhi
