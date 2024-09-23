#pragma once

#include <radray/rhi/shader_compiler.h>

#include <string>
#include <string_view>
#include <optional>
#include <variant>
#include <span>

std::optional<std::wstring> MToWideChar(std::string_view str) noexcept;

struct DxilData {
    RadrayCompilerBlob Data;
    RadrayCompilerBlob Refl;
};

using CompileResultDxil = std::variant<DxilData, std::string>;

class ShaderCompilerImpl {
public:
    class DxcImpl;
    class MscImpl;

    ShaderCompilerImpl(const RadrayShaderCompilerCreateDescriptor* desc) noexcept;
    ~ShaderCompilerImpl() noexcept;

    RadrayCompilerBlob CreateBlob(const void* data, size_t size) const noexcept;
    void DestroyBlob(RadrayCompilerBlob blob) const noexcept;

    CompileResultDxil DxcCompileHlsl(std::string_view code, std::span<std::string_view> args) const noexcept;

private:
    RadrayShaderCompilerCreateDescriptor _desc{};

    DxcImpl* _dxc{nullptr};
    MscImpl* _msc{nullptr};
};
