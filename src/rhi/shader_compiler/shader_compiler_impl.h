#pragma once

#include <radray/rhi/shader_compiler.h>

#include <string>
#include <string_view>
#include <optional>
#include <variant>
#include <span>

std::optional<std::wstring> MToWideChar(std::string_view str) noexcept;

template <typename Call>
class MScopeGuard {
public:
    explicit constexpr MScopeGuard(Call&& f) noexcept : _fun(std::forward<Call>(f)), _active(true) {}
    constexpr MScopeGuard(MScopeGuard&& rhs) noexcept : _fun(std::move(rhs._fun)), _active(rhs._active) {
        rhs.Dismiss();
    }
    constexpr ~MScopeGuard() noexcept {
        if (_active) {
            _fun();
        }
    }
    MScopeGuard() = delete;
    MScopeGuard(const MScopeGuard&) = delete;
    MScopeGuard& operator=(const MScopeGuard&) = delete;

    constexpr void Dismiss() noexcept { _active = false; }

private:
    Call _fun;
    bool _active;
};

struct DxilData {
    RadrayCompilerBlob Data;
    RadrayCompilerBlob Refl;
};

using CompileResultDxil = std::variant<DxilData, std::string>;
using ConvertResultMetallib = std::variant<RadrayCompilerBlob, std::string>;

class ShaderCompilerImpl {
public:
    class DxcImpl;
    class MscImpl;

    ShaderCompilerImpl(const RadrayShaderCompilerCreateDescriptor* desc) noexcept;
    ~ShaderCompilerImpl() noexcept;

    RadrayCompilerBlob CreateBlob(const void* data, size_t size) const noexcept;
    void DestroyBlob(RadrayCompilerBlob blob) const noexcept;
    void Log(RadrayShaderCompilerLogLevel level, std::string_view log) const noexcept;

    CompileResultDxil DxcCompileHlsl(std::string_view code, std::span<std::string_view> args) const noexcept;
    ConvertResultMetallib MscConvertHlslToMetallib(std::span<const uint8_t> dxil, RadrayShaderCompilerMetalStage stage) const noexcept;

public:
    RadrayShaderCompilerCreateDescriptor _desc{};

    DxcImpl* _dxc{nullptr};
    MscImpl* _msc{nullptr};
};
