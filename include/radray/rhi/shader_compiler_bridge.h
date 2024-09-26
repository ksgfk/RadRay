#pragma once

#include <variant>
#include <span>

#include <radray/types.h>
#include <radray/platform.h>
#include <radray/utility.h>
#include <radray/rhi/ctypes.h>
#include <radray/rhi/shader_compiler.h>

namespace radray::rhi {

class ShaderCompilerBridge;

class CompilerBlob {
public:
    ~CompilerBlob() noexcept;
    RADRAY_NO_COPY_CTOR(CompilerBlob);
    CompilerBlob(CompilerBlob&& other) noexcept;
    CompilerBlob& operator=(CompilerBlob&& other) noexcept;

    std::span<const uint8_t> GetView() const noexcept;

    friend void swap(CompilerBlob& l, CompilerBlob& r) noexcept {
        std::swap(l._sc, r._sc);
        std::swap(l._blob, r._blob);
    }

private:
    CompilerBlob(radray::shared_ptr<const ShaderCompilerBridge> sc, RadrayCompilerBlob blob);

    radray::shared_ptr<const ShaderCompilerBridge> _sc{};
    RadrayCompilerBlob _blob{};

    friend class ShaderCompilerBridge;
};

class CompilerError {
public:
    ~CompilerError() noexcept;
    RADRAY_NO_COPY_CTOR(CompilerError);
    CompilerError(CompilerError&& other) noexcept;
    CompilerError& operator=(CompilerError&& other) noexcept;

    std::string_view GetView() const noexcept;

    friend void swap(CompilerError& l, CompilerError& r) noexcept {
        std::swap(l._sc, r._sc);
        std::swap(l._err, r._err);
    }

private:
    CompilerError(radray::shared_ptr<const ShaderCompilerBridge> sc, RadrayCompilerError err);

    radray::shared_ptr<const ShaderCompilerBridge> _sc{};
    RadrayCompilerError _err{};

    friend class ShaderCompilerBridge;
};

class DxilWithReflection {
public:
    CompilerBlob Dxil;
    CompilerBlob Refl;
};

using DxcCompilerResult = std::variant<DxilWithReflection, radray::string>;

using DxcCreateReflectionResult = std::variant<ID3D12ShaderReflection*, radray::string>;

using MscConvertResult = std::variant<CompilerBlob, radray::string>;

class ShaderCompilerBridge : public radray::enable_shared_from_this<ShaderCompilerBridge> {
public:
    ShaderCompilerBridge();
    ~ShaderCompilerBridge() noexcept;
    RADRAY_NO_COPY_CTOR(ShaderCompilerBridge);
    ShaderCompilerBridge(ShaderCompilerBridge&& other) noexcept;
    ShaderCompilerBridge& operator=(ShaderCompilerBridge&& other) noexcept;

    bool IsValid() const noexcept;

    bool IsAvailable(RadrayShaderCompilerType type) const noexcept;

    DxcCompilerResult DxcHlslToDxil(std::span<const char> hlsl, std::span<std::string_view> args) const noexcept;

    DxcCompilerResult DxcHlslToDxil(const RadrayCompileRasterizationShaderDescriptor& desc) const noexcept;

    DxcCreateReflectionResult DxcCreateReflection(std::span<const uint8_t> dxil) const noexcept;

    MscConvertResult MscDxilToMetallib(std::span<const uint8_t> dxil, RadrayShaderCompilerMetalStage stage) const noexcept;

    friend constexpr void swap(ShaderCompilerBridge& l, ShaderCompilerBridge& r) noexcept {
        swap(l._scLib, r._scLib);
        std::swap(l._shaderCompiler, r._shaderCompiler);
    }

private:
    void DestroyShaderBlob(RadrayCompilerBlob blob) const noexcept;
    void DestroyError(RadrayCompilerError error) const noexcept;

    DynamicLibrary _scLib;
    RadrayShaderCompiler* _shaderCompiler;

    std::add_pointer_t<decltype(RadrayCreateShaderCompiler)> CreateShaderCompiler;
    std::add_pointer_t<decltype(RadrayReleaseShaderCompiler)> ReleaseShaderCompiler;

    friend class CompilerBlob;
    friend class CompilerError;
};

RadrayShaderCompilerMetalStage ToMscStage(RadrayShaderStage stage) noexcept;

}  // namespace radray::rhi
