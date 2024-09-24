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
      _dxc(new DxcImpl{this}) {
#ifdef RADRAYSC_ENABLE_MSC
    _msc = new MscImpl{this};
#endif
}

ShaderCompilerImpl::~ShaderCompilerImpl() noexcept {
    delete _dxc;
#ifdef RADRAYSC_ENABLE_MSC
    delete _msc;
#endif
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
    if (_dxc == nullptr) {
        return std::string{"dxc is invalid"};
    }
    return _dxc->DxcCompileHlsl(code, args);
}

ConvertResultMetallib ShaderCompilerImpl::MscConvertHlslToMetallib(std::span<const uint8_t> dxil, RadrayShaderCompilerMetalStage stage) const noexcept {
    if (_msc == nullptr) {
        return std::string{"metal-irconverter is invalid"};
    }
#ifdef RADRAYSC_ENABLE_MSC
    return _msc->DxilToMetallib(dxil, stage);
#else
    (void)dxil;
    (void)stage;
    return std::string{"metal-irconverter is invalid"};
#endif
}

struct ShaderCompilerInterface {
    RadrayShaderCompiler ctype;
    ShaderCompilerImpl* impl;
};

static ShaderCompilerInterface* Underlaying(RadrayShaderCompiler* this_) noexcept { return reinterpret_cast<ShaderCompilerInterface*>(this_); }

static bool IsCompilerAvailableImpl(RadrayShaderCompiler* this_, RadrayShaderCompilerType type) noexcept {
    auto sc = Underlaying(this_);
    switch (type) {
        case RADRAY_SHADER_COMPILER_DXC: return sc->impl->_dxc != nullptr;
        case RADRAY_SHADER_COMPILER_MSC: return sc->impl->_msc != nullptr;
        case RADRAY_SHADER_COMPILER_SPIRV_CROSS: return true;
    }
    return false;
}

static void CreateCompilerErrorImpl(RadrayCompilerError* error, const std::string& str) noexcept {
    auto newStr = new char[str.size()];
    error->Str = newStr;
    error->StrSize = str.size();
    std::copy(str.begin(), str.end(), newStr);
}

static void DestroyCompilerErrorImpl(RadrayShaderCompiler* this_, RadrayCompilerError* error) noexcept {
    delete[] error->Str;
    error->Str = nullptr;
    error->StrSize = 0;
    (void)this_;
}

static void DestroyCompilerBlobImpl(RadrayShaderCompiler* this_, RadrayCompilerBlob* blob) noexcept {
    auto sc = Underlaying(this_);
    sc->impl->DestroyBlob(*blob);
    blob->Data = nullptr;
    blob->DataSize = 0;
}

static bool DxcCompileHlslToDxilImpl(
    RadrayShaderCompiler* this_,
    const char* hlslCode, size_t codeSize,
    const char* const* args, size_t argCount,
    RadrayCompilerBlob* dxil, RadrayCompilerBlob* refl,
    RadrayCompilerError* error) noexcept {
    auto sc = Underlaying(this_);
    std::vector<std::string_view> margs;
    margs.reserve(argCount);
    for (size_t i = 0; i < argCount; i++) {
        margs.emplace_back(std::string_view{args[i]});
    }
    CompileResultDxil result = sc->impl->DxcCompileHlsl(std::string_view{hlslCode, codeSize}, margs);
    if (auto errStr = std::get_if<std::string>(&result)) {
        if (error != nullptr) {
            CreateCompilerErrorImpl(error, *errStr);
        }
        return false;
    } else if (auto data = std::get_if<DxilData>(&result)) {
        *dxil = data->Data;
        *refl = data->Refl;
        return true;
    }
    CreateCompilerErrorImpl(error, "internal error");
    return false;
}

static bool MscConvertDxilToMetallibImpl(
    RadrayShaderCompiler* this_,
    const uint8_t* dxilCode, size_t codeSize,
    RadrayShaderCompilerMetalStage stage,
    RadrayCompilerBlob* metallib,
    RadrayCompilerError* error) noexcept {
    auto sc = Underlaying(this_);
    ConvertResultMetallib result = sc->impl->MscConvertHlslToMetallib(std::span<const uint8_t>{dxilCode, codeSize}, stage);
    if (auto errStr = std::get_if<std::string>(&result)) {
        if (error != nullptr) {
            CreateCompilerErrorImpl(error, *errStr);
        }
        return false;
    } else if (auto data = std::get_if<RadrayCompilerBlob>(&result)) {
        *metallib = *data;
        return true;
    }
    CreateCompilerErrorImpl(error, "internal error");
    return false;
}

RadrayShaderCompiler* RadrayCreateShaderCompiler(const RadrayShaderCompilerCreateDescriptor* desc) RADRAYSC_NOEXCEPT {
    auto sc = new ShaderCompilerInterface{};
    sc->impl = new ShaderCompilerImpl{desc};
    sc->ctype = {
        IsCompilerAvailableImpl,
        DestroyCompilerErrorImpl,
        DestroyCompilerBlobImpl,
        DxcCompileHlslToDxilImpl,
        MscConvertDxilToMetallibImpl};
    return &sc->ctype;
}

void RadrayReleaseShaderCompiler(RadrayShaderCompiler* sc) RADRAYSC_NOEXCEPT {
    auto s = reinterpret_cast<ShaderCompilerInterface*>(sc);
    delete s->impl;
    delete s;
}
