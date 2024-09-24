#include <radray/rhi/shader_compiler_bridge.h>

#include <string_view>

#include <radray/logger.h>

namespace radray::rhi {

ShaderCompilerBridge::ShaderCompilerBridge()
    : _scLib("radray_shader_compiler") {
    if (_scLib.IsValid()) {
        CreateShaderCompiler = _scLib.GetFunction<decltype(RadrayCreateShaderCompiler)>("RadrayCreateShaderCompiler");
        ReleaseShaderCompiler = _scLib.GetFunction<decltype(RadrayReleaseShaderCompiler)>("RadrayReleaseShaderCompiler");
        RadrayShaderCompilerCreateDescriptor desc{};
        desc.Log = [](RadrayShaderCompilerLogLevel level, const char* str, size_t length, void* userPtr) noexcept {
            switch (level) {
                case RADRAY_SHADER_COMPILER_LOG_DEBUG: RADRAY_DEBUG_LOG("{}", std::string_view{str, str + length}); break;
                case RADRAY_SHADER_COMPILER_LOG_INFO: RADRAY_INFO_LOG("{}", std::string_view{str, str + length}); break;
                case RADRAY_SHADER_COMPILER_LOG_ERROR: RADRAY_ERR_LOG("{}", std::string_view{str, str + length}); break;
            }
            RADRAY_UNUSED(userPtr);
        };
        desc.UserPtr = nullptr;
        _shaderCompiler = CreateShaderCompiler(&desc);
    }
}

ShaderCompilerBridge::~ShaderCompilerBridge() noexcept {
    if (_scLib.IsValid() && _shaderCompiler != nullptr) {
        ReleaseShaderCompiler(_shaderCompiler);
        _shaderCompiler = nullptr;
    }
}

bool ShaderCompilerBridge::IsAvailable(RadrayShaderCompilerType type) const noexcept {
    return _shaderCompiler->IsAvailable(_shaderCompiler, type);
}

}  // namespace radray::rhi
