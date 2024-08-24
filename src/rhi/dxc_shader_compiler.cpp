#include <radray/rhi/dxc_shader_compiler.h>

#include <stdexcept>
#include <type_traits>
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
            RADRAY_THROW(std::runtime_error, "cannot load dxcompiler");
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

    CompileResult Compile(std::string_view code, std::span<LPCWSTR> args) {
        return "no impl";
    }

private:
    DynamicLibrary _dxcLib;
    RhiComPtr<IDxcCompiler> _dxc;
    RhiComPtr<IDxcUtils> _utils;
};

DxcShaderCompiler::DxcShaderCompiler() : _impl(radray::make_unique<DxcShaderCompiler::Impl>()) {}

CompileResult DxcShaderCompiler::Compile(std::string_view code, std::span<const wchar_t*> args) const { return _impl->Compile(code, args); }

}  // namespace radray::rhi
