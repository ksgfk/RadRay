#include <radray/rhi/shader_compiler_bridge.h>

#include <sstream>
#include <string_view>

#include <radray/logger.h>

namespace radray::rhi {

CompilerBlob::CompilerBlob(radray::shared_ptr<const ShaderCompilerBridge> sc, RadrayCompilerBlob blob)
    : _sc(sc), _blob(blob) {}

CompilerBlob::~CompilerBlob() noexcept {
    if (_blob.Data != nullptr) {
        _sc->DestroyShaderBlob(_blob);
        _blob.Data = nullptr;
    }
}

CompilerBlob::CompilerBlob(CompilerBlob&& other) noexcept
    : _sc(std::move(other._sc)),
      _blob(other._blob) {
    other._blob = {nullptr, 0};
}

CompilerBlob& CompilerBlob::operator=(CompilerBlob&& other) noexcept {
    CompilerBlob temp{std::move(other)};
    swap(*this, temp);
    return *this;
}

std::span<const uint8_t> CompilerBlob::GetView() const noexcept {
    return std::span<const uint8_t>{_blob.Data, _blob.DataSize};
}

CompilerError::CompilerError(radray::shared_ptr<const ShaderCompilerBridge> sc, RadrayCompilerError err)
    : _sc(sc), _err(err) {}

CompilerError::~CompilerError() noexcept {
    if (_err.Str != nullptr) {
        _sc->DestroyError(_err);
        _err.Str = nullptr;
    }
}

CompilerError::CompilerError(CompilerError&& other) noexcept
    : _sc(std::move(other._sc)),
      _err(other._err) {
    other._err = {nullptr, 0};
}

CompilerError& CompilerError::operator=(CompilerError&& other) noexcept {
    CompilerError temp{std::move(other)};
    swap(*this, temp);
    return *this;
}

std::string_view CompilerError::GetView() const noexcept {
    return std::string_view{_err.Str, _err.StrSize};
}

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

ShaderCompilerBridge::ShaderCompilerBridge(ShaderCompilerBridge&& other) noexcept
    : _scLib(std::move(other._scLib)),
      _shaderCompiler(other._shaderCompiler) {
    other._shaderCompiler = nullptr;
}

ShaderCompilerBridge& ShaderCompilerBridge::operator=(ShaderCompilerBridge&& other) noexcept {
    ShaderCompilerBridge temp{std::move(other)};
    swap(*this, temp);
    return *this;
}

bool ShaderCompilerBridge::IsValid() const noexcept {
    return _scLib.IsValid();
}

bool ShaderCompilerBridge::IsAvailable(RadrayShaderCompilerType type) const noexcept {
    return _shaderCompiler->IsAvailable(_shaderCompiler, type);
}

DxcCompilerResult ShaderCompilerBridge::DxcHlslToDxil(std::span<const char> hlsl, std::span<std::string_view> args) const noexcept {
    if (!IsValid()) {
        return radray::string{"radray shader compiler is invalid"};
    }
    radray::vector<const char*> temp;
    temp.reserve(args.size());
    for (auto&& arg : args) {
        temp.emplace_back(arg.data());
    }
    RadrayCompilerBlob hlslBlob{};
    RadrayCompilerBlob reflBlob{};
    RadrayCompilerError err{};
    bool result = _shaderCompiler->CompileHlslToDxil(
        _shaderCompiler,
        hlsl.data(), hlsl.size(),
        temp.data(), temp.size(),
        &hlslBlob, &reflBlob,
        &err);
    CompilerBlob radHlslBlob{shared_from_this(), hlslBlob};
    CompilerBlob radReflBlob{shared_from_this(), reflBlob};
    CompilerError radErr{shared_from_this(), err};
    if (result) {
        return DxilWithReflection{std::move(radHlslBlob), std::move(radReflBlob)};
    } else {
        return radray::string{radErr.GetView()};
    }
}

DxcCompilerResult ShaderCompilerBridge::DxcHlslToDxil(const RadrayCompileRasterizationShaderDescriptor& desc) const noexcept {
    auto toSm = [](RadrayShaderStage stage, uint32_t shaderModel) {
        using oss = std::basic_ostringstream<char, std::char_traits<char>, radray::allocator<char>>;
        oss s{};
        switch (stage) {
            case RADRAY_SHADER_STAGE_VERTEX: s << "vs_"; break;
            case RADRAY_SHADER_STAGE_HULL: s << "hs_"; break;
            case RADRAY_SHADER_STAGE_DOMAIN: s << "ds_"; break;
            case RADRAY_SHADER_STAGE_GEOMETRY: s << "gs_"; break;
            case RADRAY_SHADER_STAGE_PIXEL: s << "ps_"; break;
            case RADRAY_SHADER_STAGE_COMPUTE: s << "cs_"; break;
            default: break;
        }
        s << (shaderModel / 10) << "_" << shaderModel % 10;
        radray::string result = s.str();
        return result;
    };
    radray::string sm = toSm(desc.Stage, desc.ShaderModel);
    radray::vector<std::string_view> args{};
    args.emplace_back("-all_resources_bound");
    {
        args.emplace_back("-HV");
        args.emplace_back("2021");
    }
    if (desc.IsOptimize) {
        args.emplace_back("-O3");
    } else {
        args.emplace_back("-Od");
        args.emplace_back("-Zi");
    }
    {
        args.emplace_back("-T");
        args.emplace_back(sm);
    }
    {
        args.emplace_back("-E");
        args.emplace_back(std::string_view{desc.EntryPoint});
    }
    for (size_t i = 0; i < desc.DefineCount; i++) {
        args.emplace_back("-D");
        args.emplace_back(std::string_view{desc.Defines[i]});
    }
    for (size_t i = 0; i < desc.IncludeDirCount; i++) {
        args.emplace_back("-I");
        args.emplace_back(std::string_view{desc.IncludeDirs[i]});
    }
    return DxcHlslToDxil(std::string_view{desc.Data, desc.DataLength}, args);
}

DxcCreateReflectionResult ShaderCompilerBridge::DxcCreateReflection(std::span<const uint8_t> dxil) const noexcept {
    ID3D12ShaderReflection* result{nullptr};
    RadrayCompilerError err{};
    bool isSucc = _shaderCompiler->CreateD3D12Reflection(_shaderCompiler, dxil.data(), dxil.size(), &result, &err);
    CompilerError radErr{shared_from_this(), err};
    if (isSucc) {
        return result;
    } else {
        return radray::string{radErr.GetView()};
    }
}

MscConvertResult ShaderCompilerBridge::MscDxilToMetallib(std::span<const uint8_t> dxil, RadrayShaderCompilerMetalStage stage) const noexcept {
    RadrayCompilerBlob mtllib{};
    RadrayCompilerError err{};
    bool result = _shaderCompiler->ConvertDxilToMetallib(_shaderCompiler, dxil.data(), dxil.size(), stage, &mtllib, &err);
    CompilerBlob radBlob{shared_from_this(), mtllib};
    CompilerError radErr{shared_from_this(), err};
    if (result) {
        return radBlob;
    } else {
        return radray::string{radErr.GetView()};
    }
}

void ShaderCompilerBridge::DestroyShaderBlob(RadrayCompilerBlob blob) const noexcept {
    _shaderCompiler->DestroyBlob(_shaderCompiler, &blob);
}

void ShaderCompilerBridge::DestroyError(RadrayCompilerError error) const noexcept {
    _shaderCompiler->DestroyError(_shaderCompiler, &error);
}

RadrayShaderCompilerMetalStage ToMscStage(RadrayShaderStage stage) noexcept {
    switch (stage) {
        case RADRAY_SHADER_STAGE_UNKNOWN: return RADRAY_SHADER_COMPILER_MTL_STAGE_INVALID;
        case RADRAY_SHADER_STAGE_VERTEX: return RADRAY_SHADER_COMPILER_MTL_STAGE_VERTEX;
        case RADRAY_SHADER_STAGE_HULL: return RADRAY_SHADER_COMPILER_MTL_STAGE_HULL;
        case RADRAY_SHADER_STAGE_DOMAIN: return RADRAY_SHADER_COMPILER_MTL_STAGE_DOMAIN;
        case RADRAY_SHADER_STAGE_GEOMETRY: return RADRAY_SHADER_COMPILER_MTL_STAGE_GEOMETRY;
        case RADRAY_SHADER_STAGE_PIXEL: return RADRAY_SHADER_COMPILER_MTL_STAGE_PIXEL;
        case RADRAY_SHADER_STAGE_COMPUTE: return RADRAY_SHADER_COMPILER_MTL_STAGE_COMPUTE;
        case RADRAY_SHADER_STAGE_RAYTRACING: return RADRAY_SHADER_COMPILER_MTL_STAGE_INVALID;
        case RADRAY_SHADER_STAGE_ALL_GRAPHICS: return RADRAY_SHADER_COMPILER_MTL_STAGE_INVALID;
    }
}

}  // namespace radray::rhi
