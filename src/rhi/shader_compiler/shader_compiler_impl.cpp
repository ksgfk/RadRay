#include "shader_compiler_impl.h"

#include <vector>

#include "directx_shader_compiler.h"
#ifdef RADRAYSC_ENABLE_MSC
#include "metal_ir_converter.h"
#endif

std::optional<std::wstring> MToWideChar(std::string_view str) noexcept {
#ifdef _WIN64
    const char* mbstr = str.data();
    std::mbstate_t state{};
    size_t len{0};
    mbsrtowcs_s(&len, nullptr, 0, &mbstr, str.size(), &state);
    if (len != str.size() + 1) {  // because string_view dose not contains eof '\0'
        return std::nullopt;
    }
    std::vector<wchar_t> wstr(len);
    state = mbstate_t{};
    auto err = mbsrtowcs_s(nullptr, wstr.data(), wstr.size(), &mbstr, len, &state);
    if (err == 0) {
        return std::wstring{wstr.begin(), wstr.end()};
    } else {
        return std::nullopt;
    }
#else
    const char* start = str.data();
    std::mbstate_t state{};
    size_t len = std::mbsrtowcs(nullptr, &start, str.size(), &state);
    if (len != str.size()) {
        return std::nullopt;
    }
    std::vector<wchar_t> wstr(len + 1);
    state = mbstate_t{};
    size_t result = std::mbsrtowcs(&wstr[0], &start, wstr.size(), &state);
    if (result == (size_t)-1) {
        return std::nullopt;
    } else {
        return std::wstring{wstr.begin(), wstr.end()};
    }
#endif
}

ShaderCompilerImpl::ShaderCompilerImpl(const RadrayShaderCompilerCreateDescriptor* desc) noexcept
    : _desc(*desc),
      _dxc(new DxcImpl{this}) {}

ShaderCompilerImpl::~ShaderCompilerImpl() noexcept {
    delete _dxc;
}

RadrayCompilerBlob ShaderCompilerImpl::CreateBlob(const void* data, size_t size) const noexcept {
    auto dst = new uint8_t[size];
    std::memcpy(dst, data, size);
    return {dst, size};
}

void ShaderCompilerImpl::DestroyBlob(RadrayCompilerBlob blob) const noexcept {
    delete[] blob.Data;
}

void ShaderCompilerImpl::Log(RadrayShaderCompilerLogLevel level, std::string_view log) const noexcept {
    if (_desc.Log) {
        _desc.Log(level, log.data(), log.size(), _desc.UserPtr);
    }
}

CompileResultDxil ShaderCompilerImpl::DxcCompileHlsl(std::string_view code, std::span<std::string_view> args) const noexcept {
    return _dxc->DxcCompileHlsl(code, args);
}

struct ShaderCompilerInterface {
    RadrayShaderCompiler ctype;
    ShaderCompilerImpl* impl;
};

RadrayShaderCompiler* RadrayCreateShaderCompiler(const RadrayShaderCompilerCreateDescriptor* desc) RADRAYSC_NOEXCEPT {
    auto sc = new ShaderCompilerInterface{};
    sc->impl = new ShaderCompilerImpl{desc};
    return &sc->ctype;
}

void RadrayReleaseShaderCompiler(RadrayShaderCompiler* sc) RADRAYSC_NOEXCEPT {
    auto s = reinterpret_cast<ShaderCompilerInterface*>(sc);
    delete s->impl;
    delete s;
}
