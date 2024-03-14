#include <radray/d3d12/shader_compiler.h>

namespace radray::d3d12 {

class DefaultIncludeHandler : public IDxcIncludeHandler {
public:
    DefaultIncludeHandler(const ShaderCompiler* sc) : _sc(sc) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(
        /* [in] */ REFIID riid,
        /* [iid_is][out] */ _COM_Outptr_ void __RPC_FAR* __RPC_FAR* ppvObject) override {
        *ppvObject = this;
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef(void) override { return S_OK; }
    ULONG STDMETHODCALLTYPE Release(void) override { return S_OK; }

    HRESULT STDMETHODCALLTYPE LoadSource(
        _In_z_ LPCWSTR pFilename,
        _COM_Outptr_result_maybenull_ IDxcBlob** ppIncludeSource) override {
        for (auto&& i : _sc->includeDirs) {
            std::filesystem::path fullName = i / std::filesystem::path{pFilename};
            if (!std::filesystem::exists(fullName)) {
                continue;
            }
            auto&& wname = fullName.generic_wstring();
            UINT32 codepage = CP_ACP;
            IDxcBlobEncoding* result;
            HRESULT hr = _sc->utils->LoadFile(wname.c_str(), &codepage, &result);
            if (SUCCEEDED(hr)) {
                *ppIncludeSource = result;
                return S_OK;
            }
        }
        *ppIncludeSource = nullptr;
        return -1;
    };

private:
    const ShaderCompiler* _sc;
};

ShaderCompiler::ShaderCompiler() noexcept {
    ThrowIfFailed(DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(compiler.GetAddressOf())));
    ThrowIfFailed(DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(utils.GetAddressOf())));
}

ShaderCompileResult ShaderCompiler::Compile(std::string_view code, std::span<LPCWSTR> args) const {
    DxcBuffer buffer{
        code.data(),
        code.size(),
        CP_ACP};
    ComPtr<IDxcResult> compileResult;
    DefaultIncludeHandler ih{this};
    ThrowIfFailed(compiler->Compile(
        &buffer,
        args.data(),
        args.size(),
        &ih,
        IID_PPV_ARGS(compileResult.GetAddressOf())));
    HRESULT status;
    ThrowIfFailed(compileResult->GetStatus(&status));
    if (status == 0) {
        ComPtr<IDxcBlob> resultBlob;
        ThrowIfFailed(compileResult->GetResult(resultBlob.GetAddressOf()));
        return {std::move(resultBlob), std::string{}};
    } else {
        ComPtr<IDxcBlobEncoding> errBuffer;
        ThrowIfFailed(compileResult->GetErrorBuffer(errBuffer.GetAddressOf()));
        std::wstring_view errStr{reinterpret_cast<wchar_t const*>(errBuffer->GetBufferPointer()), errBuffer->GetBufferSize()};
        return {nullptr, Utf8ToString(std::wstring{errStr})};
    }
}

ShaderCompileResult ShaderCompiler::Compile(
    std::string_view code,
    std::string_view entryPoint,
    std::string_view shaderModel,
    bool optimize) const {
    std::vector<LPCWSTR> args;
    args.emplace_back(DXC_ARG_ALL_RESOURCES_BOUND);
    args.emplace_back(DXC_ARG_PACK_MATRIX_COLUMN_MAJOR);
    args.emplace_back(L"-HV 2021");
    if (optimize) {
        args.emplace_back(DXC_ARG_OPTIMIZATION_LEVEL3);
    }
    args.emplace_back(L"/T");
    std::wstring shaderModelW = Utf8ToWString(std::string{shaderModel});
    args.emplace_back(shaderModelW.data());
    args.emplace_back(L"/E");
    std::wstring entryPointW = Utf8ToWString(std::string{entryPoint});
    args.emplace_back(entryPointW.data());
    return Compile(code, std::span<LPCWSTR>{args.data(), args.size()});
}

}  // namespace radray::d3d12
