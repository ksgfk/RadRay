#include <radray/render/dxc.h>

#ifdef RADRAY_ENABLE_DXC

#include <atomic>
#include <sstream>

#include <radray/logger.h>
#include <radray/utility.h>

#ifndef RADRAY_PLATFORM_WINDOWS
#define __EMULATE_UUID
#include <WinAdapter.h>
#endif
#include <dxcapi.h>

#ifdef RADRAY_PLATFORM_WINDOWS
using Microsoft::WRL::ComPtr;
#else
template <class T>
using ComPtr = CComPtr<T>;
#endif

namespace radray::render {

#ifdef RADRAY_ENABLE_MIMALLOC
class MiMallocAdapter : public IMalloc {
public:
    virtual ~MiMallocAdapter() noexcept = default;

    HRESULT QueryInterface(REFIID riid, void** ppvObject) override {
        if (riid == __uuidof(IMalloc)) {
            *ppvObject = static_cast<IMalloc*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG AddRef() override {
        return ++_refCnt;
    }
    ULONG Release() override {
        auto count = --_refCnt;
        if (count == 0) {
            delete this;
        }
        return count;
    }

    void* Alloc(SIZE_T cb) override { return mi_malloc(cb); }
    void* Realloc(void* pv, SIZE_T cb) override { return mi_realloc(pv, cb); }
    void Free(void* pv) override { mi_free(pv); }
    SIZE_T GetSize(void* pv) override { return mi_usable_size(pv); }
    int DidAlloc(void* pv) override { return pv == nullptr ? -1 : (mi_is_in_heap_region(pv) ? 1 : 0); }
    void HeapMinimize() override { mi_collect(true); }

private:
    std::atomic<ULONG> _refCnt;
};
#endif

class Dxc::Impl : public Noncopyable {
public:
    Impl() noexcept = default;

    std::optional<DxilBlob> Compile(std::string_view code, std::span<std::string_view> args) noexcept {
        radray::vector<radray::wstring> wargs;
        wargs.reserve(args.size());
        for (auto i : args) {
            auto w = ToWideChar(i);
            if (!w.has_value()) {
                RADRAY_ERR_LOG("cannot convert to wide str: {}", i);
                return std::nullopt;
            }
            wargs.emplace_back(std::move(w.value()));
        }
        radray::vector<LPCWSTR> argsref;
        argsref.reserve(wargs.size());
        for (auto&& i : wargs) {
            argsref.emplace_back(i.c_str());
        }
        DxcBuffer buffer{code.data(), code.size(), CP_ACP};
        ComPtr<IDxcResult> compileResult;
        if (HRESULT hr = _dxc->Compile(
                &buffer,
                argsref.data(),
                argsref.size(),
                _inc,
                IID_PPV_ARGS(&compileResult));
            hr != S_OK) {
            RADRAY_ERR_LOG("dxc error, code={}", hr);
            return std::nullopt;
        }
        HRESULT status;
        if (HRESULT hr = compileResult->GetStatus(&status);
            hr != S_OK) {
            RADRAY_ERR_LOG("dxc error, code={}", hr);
            return std::nullopt;
        }
        if (status != S_OK) {
            ComPtr<IDxcBlobEncoding> errBuffer;
            if (HRESULT hr = compileResult->GetErrorBuffer(&errBuffer);
                hr != S_OK) {
                RADRAY_ERR_LOG("dxc error, code={}", hr);
                return std::nullopt;
            }
            std::string_view errStr{reinterpret_cast<char const*>(errBuffer->GetBufferPointer()), errBuffer->GetBufferSize()};
            RADRAY_ERR_LOG("compile hlsl error\n{}", errStr);
            return std::nullopt;
        }
        ComPtr<IDxcBlob> dxilBlob;
        if (HRESULT hr = compileResult->GetResult(&dxilBlob);
            hr != S_OK) {
            RADRAY_ERR_LOG("dxc error, code={}", hr);
            return std::nullopt;
        }
        ComPtr<IDxcBlob> reflBlob;
        if (HRESULT hr = compileResult->GetOutput(DXC_OUT_REFLECTION, IID_PPV_ARGS(&reflBlob), nullptr);
            hr != S_OK) {
            RADRAY_ERR_LOG("dxc error, code={}", hr);
            return std::nullopt;
        }
        auto dxilStart = reinterpret_cast<byte const*>(dxilBlob->GetBufferPointer());
        auto reflStart = reinterpret_cast<byte const*>(reflBlob->GetBufferPointer());
        return DxilBlob{
            .data = {dxilStart, dxilStart + dxilBlob->GetBufferSize()},
            .refl = {reflStart, reflStart + reflBlob->GetBufferSize()}};
    }

public:
#ifdef RADRAY_ENABLE_MIMALLOC
    ComPtr<MiMallocAdapter> _mi;
#endif
    ComPtr<IDxcCompiler3> _dxc;
    ComPtr<IDxcUtils> _utils;
    ComPtr<IDxcIncludeHandler> _inc;
};

std::optional<std::shared_ptr<Dxc>> CreateDxc() noexcept {
    ComPtr<IDxcCompiler3> dxc;
#if RADRAY_ENABLE_MIMALLOC
    ComPtr<MiMallocAdapter> mi{new MiMallocAdapter{}};
    if (HRESULT hr = DxcCreateInstance2(mi, CLSID_DxcCompiler, IID_PPV_ARGS(&dxc));
        hr != S_OK) {
        RADRAY_ERR_LOG("cannot create IDxcCompiler3, code={}", hr);
        return std::nullopt;
    }
#else
    if (HRESULT hr = DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&dxc));
        hr != S_OK) {
        RADRAY_ERR_LOG("cannot create IDxcCompiler3, code={}", hr);
        return std::nullopt;
    }
#endif
    ComPtr<IDxcUtils> utils;
    if (HRESULT hr = DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&utils));
        hr != S_OK) {
        RADRAY_ERR_LOG("cannot create IDxcUtils, code={}", hr);
        return std::nullopt;
    }
    ComPtr<IDxcIncludeHandler> incHandler;
    if (HRESULT hr = utils->CreateDefaultIncludeHandler(&incHandler);
        hr != S_OK) {
        RADRAY_ERR_LOG("cannot create IDxcIncludeHandler, code={}", hr);
        return std::nullopt;
    }
    auto result = std::make_shared<Dxc>();
    result->_impl = new Dxc::Impl{};
#ifdef RADRAY_ENABLE_MIMALLOC
    result->_impl->_mi = std::move(mi);
#endif
    result->_impl->_dxc = std::move(dxc);
    result->_impl->_utils = std::move(utils);
    result->_impl->_inc = std::move(incHandler);
    return result;
}

Dxc::~Dxc() noexcept {
    if (_impl != nullptr) {
        delete _impl;
        _impl = nullptr;
    }
}

void Dxc::Destroy() noexcept {
    if (_impl != nullptr) {
        delete _impl;
        _impl = nullptr;
    }
}

std::optional<DxilBlob> Dxc::Compile(std::string_view code, std::span<std::string_view> args) noexcept {
    return _impl->Compile(code, args);
}

std::optional<DxilBlob> Dxc::Compile(
    std::string_view code,
    std::string_view entryPoint,
    ShaderStage stage,
    HlslShaderModel sm,
    bool isOptimize,
    std::span<std::string_view> defines,
    std::span<std::string_view> includes) noexcept {
    radray::string smStr = ([stage, sm]() noexcept {
        using oss = std::basic_ostringstream<char, std::char_traits<char>, radray::allocator<char>>;
        oss s{};
        switch (stage) {
            case ShaderStage::Vertex: s << "vs_"; break;
            case ShaderStage::Pixel: s << "ps_"; break;
            case ShaderStage::Compute: s << "cs_"; break;
        }
        switch (sm) {
            case HlslShaderModel::SM60: s << "6_0"; break;
            case HlslShaderModel::SM61: s << "6_1"; break;
            case HlslShaderModel::SM62: s << "6_2"; break;
            case HlslShaderModel::SM63: s << "6_3"; break;
            case HlslShaderModel::SM64: s << "6_4"; break;
            case HlslShaderModel::SM65: s << "6_5"; break;
            case HlslShaderModel::SM66: s << "6_6"; break;
        }
        radray::string result = s.str();
        return result;
    })();
    radray::vector<std::string_view> args{};
    args.emplace_back("-all_resources_bound");
    {
        args.emplace_back("-HV");
        args.emplace_back("2021");
    }
    if (isOptimize) {
        args.emplace_back("-O3");
    } else {
        args.emplace_back("-Od");
        args.emplace_back("-Zi");
    }
    {
        args.emplace_back("-T");
        args.emplace_back(smStr);
    }
    {
        args.emplace_back("-E");
        args.emplace_back(entryPoint);
    }
    for (auto&& i : includes) {
        args.emplace_back("-I");
        args.emplace_back(i);
    }
    for (auto&& i : defines) {
        args.emplace_back("-D");
        args.emplace_back(i);
    }
    return _impl->Compile(code, args);
}

}  // namespace radray::render

#endif
