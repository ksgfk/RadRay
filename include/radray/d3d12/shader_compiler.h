#pragma once

#include <span>
#include <filesystem>
#include <radray/d3d12/utility.h>
#include <dxcapi.h>
#include <d3d12shader.h>

namespace radray::d3d12 {

struct ShaderCompileResult {
    ComPtr<IDxcBlob> data;
    ComPtr<ID3D12ShaderReflection> refl;
    std::string error;
};

struct RasterShaderCompileResult {
    ShaderCompileResult vs;
    ShaderCompileResult ps;
};

class ShaderCompiler {
public:
    ShaderCompiler() noexcept;

    ShaderCompileResult Compile(
        std::string_view code,
        std::span<LPCWSTR> args) const;

    ShaderCompileResult Compile(
        std::string_view code,
        std::string_view entryPoint,
        std::string_view shaderModel,
        bool optimize) const;

    RasterShaderCompileResult CompileRaster(
        std::string_view code,
        uint32 shaderModel,
        bool optimize);

    ComPtr<IDxcCompiler3> compiler;
    ComPtr<IDxcUtils> utils;
    std::vector<std::filesystem::path> includeDirs;
};

}  // namespace radray::d3d12
