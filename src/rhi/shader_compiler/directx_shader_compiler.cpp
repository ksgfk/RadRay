#include "directx_shader_compiler.h"

#ifdef _WIN64
#include <windows.h>
#endif
#ifdef RADRAY_PLATFORM_MACOS
#define __EMULATE_UUID 1
#endif
#define DXC_API_IMPORT
#include <dxcapi.h>

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

    ULONG Reset() noexcept { return InternalRelease(); }

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

ShaderCompilerImpl::DxcImpl::DxcImpl(ShaderCompilerImpl* sc) noexcept
    : _sc(sc) {
    _dxcLib = _sc->_desc.GetLibAddr("dxcompiler");
    if (_dxcLib != nullptr) {
        auto pDxcCreateInstance = (std::add_pointer_t<decltype(DxcCreateInstance)>)_sc->_desc.GetProcAddr(_dxcLib, "DxcCreateInstance");
        {
            HRESULT hr = pDxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&_dxc));
            if (hr != S_OK) {
                std::string_view tips{"cannot create IDxcCompiler3 instance"};
                _sc->_desc.Log(tips.data(), tips.size());
            }
        }
        if (_dxc != nullptr) {
            HRESULT hr = pDxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&_dxcUtil));
            if (hr != S_OK) {
                std::string_view tips{"cannot create IDxcUtils instance"};
                _sc->_desc.Log(tips.data(), tips.size());
            }
        }
        if (_dxcUtil != nullptr) {
            _dxcUtil->CreateDefaultIncludeHandler(&_dxcInc);
        }
    }
}

ShaderCompilerImpl::DxcImpl::~DxcImpl() noexcept {
    _dxcInc->Release();
    _dxcUtil->Release();
    _dxc->Release();
    if (_dxcLib != nullptr) {
        _sc->_desc.CloseLib(_dxcLib);
        _dxcLib = nullptr;
    }
}

CompileResultDxil ShaderCompilerImpl::DxcImpl::DxcCompileHlsl(std::string_view code, std::span<std::string_view> args) const noexcept {
    if (_dxc == nullptr) {
        return "dxc is not avaliable";
    }
    std::vector<std::wstring> wargs;
    wargs.reserve(args.size());
    for (auto i : args) {
        auto w = MToWideChar(i);
        if (!w.has_value()) {
            return std::string{"arg cannot convert to wide str: "} + std::string{i};
        }
        wargs.emplace_back(std::move(w.value()));
    }
    std::vector<LPCWSTR> argsref;
    argsref.reserve(wargs.size());
    for (auto&& i : wargs) {
        argsref.emplace_back(i.c_str());
    }
    DxcBuffer buffer{
        code.data(),
        code.size(),
        CP_ACP};
    RhiComPtr<IDxcResult> compileResult;
    {
        HRESULT hr = _dxc->Compile(
            &buffer,
            argsref.data(),
            argsref.size(),
            _dxcInc,
            IID_PPV_ARGS(compileResult.GetAddressOf()));
        if (hr != S_OK) {
            return std::string{"dxc error "} + std::to_string(hr);
        }
    }
    HRESULT status;
    {
        HRESULT hr = compileResult->GetStatus(&status);
        if (hr != S_OK) {
            return std::string{"dxc error "} + std::to_string(hr);
        }
    }
    if (status == S_OK) {
        RhiComPtr<IDxcBlob> dxilBlob;
        {
            HRESULT hr = compileResult->GetResult(dxilBlob.GetAddressOf());
            if (hr != S_OK) {
                return std::string{"dxc error "} + std::to_string(hr);
            }
        }
        RhiComPtr<IDxcBlob> reflBlob;
        {
            HRESULT hr = compileResult->GetOutput(DXC_OUT_REFLECTION, IID_PPV_ARGS(reflBlob.GetAddressOf()), nullptr);
            if (hr != S_OK) {
                return std::string{"dxc error "} + std::to_string(hr);
            }
        }
        auto dxil = _sc->CreateBlob(dxilBlob->GetBufferPointer(), dxilBlob->GetBufferSize());
        auto refl = _sc->CreateBlob(reflBlob->GetBufferPointer(), reflBlob->GetBufferSize());
        return DxilData{dxil, refl};
    } else {
        RhiComPtr<IDxcBlobEncoding> errBuffer;
        {
            HRESULT hr = compileResult->GetErrorBuffer(errBuffer.GetAddressOf());
            if (hr != S_OK) {
                return std::string{"dxc error "} + std::to_string(hr);
            }
        }
        std::string_view errStr{reinterpret_cast<char const*>(errBuffer->GetBufferPointer()), errBuffer->GetBufferSize()};
        return std::string{errStr};
    }
}