#include <radray/rhi/dxc_shader_compiler.h>

#include <stdexcept>
#include <type_traits>
#include <sstream>
#include <span>

#ifdef RADRAY_PLATFORM_WINDOWS
#include <windows.h>
#endif
#ifdef RADRAY_PLATFORM_MACOS
#define __EMULATE_UUID 1
#endif
#define DXC_API_IMPORT
#include <dxcapi.h>

#include <radray/platform.h>
#include <radray/logger.h>
#include <radray/utility.h>
#include <radray/rhi/ctypes.h>

namespace radray::rhi {

template <class T>
requires std::is_base_of_v<IUnknown, T>
class RhiComPtr {
public:
    RhiComPtr() noexcept = default;
    explicit RhiComPtr(T* other) noexcept : _ptr(other) { InternalAddRef(); }
    RhiComPtr(const RhiComPtr& other) noexcept : _ptr(other._ptr) { InternalAddRef(); }
    RhiComPtr(RhiComPtr&& other) noexcept : _ptr(other._ptr) { other._ptr = nullptr; }
    RhiComPtr& operator=(const RhiComPtr& other) noexcept {
        RhiComPtr temp{other};
        swap(*this, temp);
        return *this;
    }
    RhiComPtr& operator=(RhiComPtr&& other) noexcept {
        RhiComPtr temp{std::move(other)};
        swap(*this, temp);
        return *this;
    }
    ~RhiComPtr() noexcept { InternalRelease(); }

    T* Get() const noexcept { return _ptr; }
    T* operator->() const noexcept { return _ptr; }
    T* const* GetAddressOf() const noexcept { return &_ptr; }
    T** GetAddressOf() noexcept { return &_ptr; }

    friend void swap(RhiComPtr& l, RhiComPtr& r) noexcept {
        std::swap(l._ptr, r._ptr);
    }

private:
    void InternalAddRef() const noexcept {
        if (_ptr != nullptr) {
            _ptr->AddRef();
        }
    }
    ULONG InternalRelease() noexcept {
        ULONG refCnt = 0;
        T* temp = _ptr;
        if (temp != nullptr) {
            _ptr = nullptr;
            refCnt = temp->Release();
        }
        return refCnt;
    }

    T* _ptr{nullptr};
};

class DxcShaderCompiler::Impl {
public:
    Impl() : _dxcLib("dxcompiler") {
        if (!_dxcLib.IsValid()) {
#ifdef RADRAY_ENABLE_DXC
            RADRAY_THROW(std::runtime_error, "cannot load dxcompiler");
#else
            return;
#endif
        }
        auto pDxcCreateInstance = _dxcLib.GetFunction<decltype(DxcCreateInstance)>("DxcCreateInstance");
        if (pDxcCreateInstance == nullptr) {
            RADRAY_THROW(std::runtime_error, "cannot get function DxcCreateInstance");
        }
        {
            auto hr = pDxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(_dxc.GetAddressOf()));
            if (hr != S_OK) {
                RADRAY_THROW(std::runtime_error, "cannot create IDxcCompiler. code={}", hr);
            }
        }
        {
            auto hr = pDxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(_utils.GetAddressOf()));
            if (hr != S_OK) {
                RADRAY_THROW(std::runtime_error, "cannot create IDxcUtils. code={}", hr);
            }
        }
    }
    ~Impl() noexcept = default;

    CompileResult Compile(std::string_view code, std::span<LPCWSTR> args) const {
        if (_dxc.Get() == nullptr) {
            return "cannot use dxc. dynamic lib dxcompiler is not exist";
        }
        DxcBuffer buffer{
            code.data(),
            code.size(),
            CP_ACP};
        RhiComPtr<IDxcResult> compileResult;
        {
            HRESULT hr = _dxc->Compile(
                &buffer,
                args.data(),
                args.size(),
                nullptr,
                IID_PPV_ARGS(compileResult.GetAddressOf()));
            if (hr != S_OK) {
                return radray::format("dxc error {}", hr);
            }
        }
        HRESULT status;
        {
            HRESULT hr = compileResult->GetStatus(&status);
            if (hr != S_OK) {
                return radray::format("dxc error {}", hr);
            }
        }
        if (status == S_OK) {
            RhiComPtr<IDxcBlob> resultBlob;
            {
                HRESULT hr = compileResult->GetResult(resultBlob.GetAddressOf());
                if (hr != S_OK) {
                    return radray::format("dxc error {}", hr);
                }
            }
            auto data = reinterpret_cast<const uint8_t*>(resultBlob->GetBufferPointer());
            radray::vector<uint8_t> code{data, data + resultBlob->GetBufferSize()};
            RhiComPtr<IDxcBlob> reflBlob;
            {
                HRESULT hr = compileResult->GetOutput(DXC_OUT_REFLECTION, IID_PPV_ARGS(reflBlob.GetAddressOf()), nullptr);
                if (hr != S_OK) {
                    return radray::format("dxc error {}", hr);
                }
            }
            auto refld = reinterpret_cast<const uint8_t*>(reflBlob->GetBufferPointer());
            radray::vector<uint8_t> refl{refld, refld + reflBlob->GetBufferSize()};
            return DxilShaderBlob{std::move(code), std::move(refl)};
        } else {
            RhiComPtr<IDxcBlobEncoding> errBuffer;
            {
                HRESULT hr = compileResult->GetErrorBuffer(errBuffer.GetAddressOf());
                if (hr != S_OK) {
                    return radray::format("dxc error {}", hr);
                }
            }
            std::string_view errStr{reinterpret_cast<char const*>(errBuffer->GetBufferPointer()), errBuffer->GetBufferSize()};
            return radray::string{errStr};
        }
    }

    CompileResult Compile(const RadrayCompileRasterizationShaderDescriptor* desc_) const {
        auto&& desc = *desc_;
        auto toSm = [](RadrayShaderStage stage, uint32_t shaderModel) {
            using oss = std::basic_ostringstream<wchar_t, std::char_traits<wchar_t>, radray::allocator<wchar_t>>;
            oss s{};
            switch (stage) {
                case RADRAY_SHADER_STAGE_VERTEX: s << L"vs_"; break;
                case RADRAY_SHADER_STAGE_HULL: s << L"hs_"; break;
                case RADRAY_SHADER_STAGE_DOMAIN: s << L"ds_"; break;
                case RADRAY_SHADER_STAGE_GEOMETRY: s << L"gs_"; break;
                case RADRAY_SHADER_STAGE_PIXEL: s << L"ps_"; break;
                case RADRAY_SHADER_STAGE_COMPUTE: s << L"cs_"; break;
                default: break;
            }
            s << (shaderModel / 10) << "_" << shaderModel % 10;
            radray::wstring result = s.str();
            return result;
        };
        radray::wstring sm = toSm(desc.Stage, desc.ShaderModel);
        radray::wstring entryPoint;
        {
            auto tmp = ToWideChar(radray::string{desc.EntryPoint});
            if (!tmp.has_value()) {
                return radray::format("cannot convert to wchar {}", desc.EntryPoint);
            }
            entryPoint = tmp.value();
        }
        radray::wstring name;
        {
            auto tmp = ToWideChar(radray::string{desc.Name});
            if (!tmp.has_value()) {
                return radray::format("cannot convert to wchar {}", desc.Name);
            }
            name = tmp.value();
        }
        radray::vector<radray::wstring> defines{};
        defines.reserve(desc.DefineCount);
        for (size_t i = 0; i < desc.DefineCount; i++) {
            auto tmp = ToWideChar(radray::string{desc.Defines[i]});
            if (!tmp.has_value()) {
                return radray::format("cannot convert to wchar {}", desc.Defines[i]);
            }
            defines.emplace_back(tmp.value());
        }
        radray::vector<LPCWSTR> args{};
        args.emplace_back(L"-all_resources_bound");
        {
            args.emplace_back(L"-HV");
            args.emplace_back(L"2021");
        }
        if (desc.IsOptimize) {
            args.emplace_back(L"-O3");
        } else {
            args.emplace_back(L"-Od");
        }
        {
            args.emplace_back(L"-T");
            args.emplace_back(sm.c_str());
        }
        {
            args.emplace_back(L"-E");
            args.emplace_back(entryPoint.c_str());
        }
        {
            args.emplace_back(L"-Fd");
            args.emplace_back(name.c_str());
        }
        for (auto&& i : defines) {
            args.emplace_back(L"-D");
            args.emplace_back(i.c_str());
        }
        return Compile(std::string_view{desc.Data, desc.DataLength}, args);
    }

public:
    DynamicLibrary _dxcLib;
    RhiComPtr<IDxcCompiler3> _dxc;
    RhiComPtr<IDxcUtils> _utils;
};

DxcShaderCompiler::DxcShaderCompiler() : _impl(new DxcShaderCompiler::Impl{}) {}

DxcShaderCompiler::~DxcShaderCompiler() noexcept {
    if (_impl != nullptr) {
        delete _impl;
        _impl = nullptr;
    }
}

CompileResult DxcShaderCompiler::Compile(std::string_view code, std::span<const wchar_t*> args) const { return _impl->Compile(code, args); }

CompileResult DxcShaderCompiler::Compile(const RadrayCompileRasterizationShaderDescriptor* desc) const { return _impl->Compile(desc); }

IDxcCompiler3* DxcShaderCompiler::GetCompiler() const noexcept { return _impl->_dxc.Get(); }

IDxcUtils* DxcShaderCompiler::GetUtils() const noexcept { return _impl->_utils.Get(); }

}  // namespace radray::rhi
