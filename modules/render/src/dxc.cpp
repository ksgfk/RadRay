#include <radray/render/dxc.h>

namespace radray::render {

bool operator==(const DxilReflection::Variable& lhs, const DxilReflection::Variable& rhs) noexcept {
    return lhs.Name == rhs.Name && lhs.Start == rhs.Start && lhs.Size == rhs.Size;
}

bool operator!=(const DxilReflection::Variable& lhs, const DxilReflection::Variable& rhs) noexcept {
    return !(lhs == rhs);
}

bool operator==(const DxilReflection::CBuffer& lhs, const DxilReflection::CBuffer& rhs) noexcept {
    if (lhs.Size != rhs.Size) {
        return false;
    }
    if (lhs.Vars.size() != rhs.Vars.size()) {
        return false;
    }
    size_t cnt = lhs.Vars.size();
    for (size_t i = 0; i < cnt; i++) {
        if (lhs.Vars[i] != rhs.Vars[i]) {
            return false;
        }
    }
    return lhs.Name == rhs.Name;
}

bool operator!=(const DxilReflection::CBuffer& lhs, const DxilReflection::CBuffer& rhs) noexcept {
    return !(lhs == rhs);
}

bool operator==(const DxilReflection::StaticSampler& lhs, const DxilReflection::StaticSampler& rhs) noexcept {
    return operator==(static_cast<SamplerDescriptor>(lhs), rhs) && lhs.Name == rhs.Name;
}

bool operator!=(const DxilReflection::StaticSampler& lhs, const DxilReflection::StaticSampler& rhs) noexcept {
    return !(lhs == rhs);
}

std::string_view format_as(ShaderResourceType v) noexcept {
    switch (v) {
        case ShaderResourceType::CBuffer: return "CBuffer";
        case ShaderResourceType::Texture: return "Texture";
        case ShaderResourceType::Buffer: return "Buffer";
        case ShaderResourceType::RWTexture: return "RWTexture";
        case ShaderResourceType::RWBuffer: return "RWBuffer";
        case ShaderResourceType::Sampler: return "Sampler";
        case ShaderResourceType::PushConstant: return "PushConstant";
        case ShaderResourceType::RayTracing: return "RayTracing";
        default: return "UNKNOWN";
    }
}

}  // namespace radray::render

#ifdef RADRAY_ENABLE_DXC

#include <atomic>
#include <sstream>
#include <optional>

#include <radray/logger.h>
#include <radray/utility.h>

#include <directx/d3d12shader.h>
#ifdef RADRAY_PLATFORM_WINDOWS
#include <radray/platform.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WINDOWS
#define _WINDOWS
#endif
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <windows.h>
#include <wrl.h>
using Microsoft::WRL::ComPtr;
#else
//TODO:
#endif

#include <dxcapi.h>

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

class DxcImpl : public Dxc::Impl, public Noncopyable {
public:
#ifdef RADRAY_ENABLE_MIMALLOC
    DxcImpl(
        ComPtr<MiMallocAdapter> mi,
        ComPtr<IDxcCompiler3> dxc,
        ComPtr<IDxcUtils> utils,
        ComPtr<IDxcIncludeHandler> inc) noexcept
        : _mi(std::move(mi)),
          _dxc(std::move(dxc)),
          _utils(std::move(utils)),
          _inc(std::move(inc)) {}
#else
    DxcImpl(
        ComPtr<IDxcCompiler3> dxc,
        ComPtr<IDxcUtils> utils,
        ComPtr<IDxcIncludeHandler> inc) noexcept
        : _dxc(std::move(dxc)),
          _utils(std::move(utils)),
          _inc(std::move(inc)) {}
#endif
    ~DxcImpl() noexcept override = default;

    std::optional<DxcOutput> Compile(std::string_view code, std::span<std::string_view> args) noexcept {
        bool isSpirv = false;
        bool isStripRefl = false;
        vector<wstring> wargs;
        wargs.reserve(args.size());
        for (auto i : args) {
            if (i == "-spirv") {
                isSpirv = true;
            }
            if (i == "-Qstrip_reflect") {
                isStripRefl = true;
            }
            auto w = ToWideChar(i);
            if (!w.has_value()) {
                RADRAY_ERR_LOG("cannot convert to wide str: {}", i);
                return std::nullopt;
            }
            wargs.emplace_back(std::move(w.value()));
        }
        vector<LPCWSTR> argsref;
        argsref.reserve(wargs.size());
        for (auto&& i : wargs) {
            argsref.emplace_back(i.c_str());
        }
        DxcBuffer buffer{code.data(), code.size(), CP_ACP};
        ComPtr<IDxcResult> compileResult;
        if (HRESULT hr = _dxc->Compile(
                &buffer,
                argsref.data(),
                (UINT32)argsref.size(),
                _inc.Get(),
                IID_PPV_ARGS(&compileResult));
            FAILED(hr)) {
            RADRAY_ERR_LOG("dxc error, code={}", hr);
            return std::nullopt;
        }
        HRESULT status;
        if (HRESULT hr = compileResult->GetStatus(&status);
            FAILED(hr)) {
            RADRAY_ERR_LOG("dxc error, code={}", hr);
            return std::nullopt;
        }
        if (FAILED(status)) {
            ComPtr<IDxcBlobEncoding> errBuffer;
            if (HRESULT hr = compileResult->GetErrorBuffer(&errBuffer);
                FAILED(hr)) {
                RADRAY_ERR_LOG("dxc error, code={}", hr);
                return std::nullopt;
            }
            std::string_view errStr{std::bit_cast<char const*>(errBuffer->GetBufferPointer()), errBuffer->GetBufferSize()};
            RADRAY_ERR_LOG("dxc compile error\n{}", errStr);
            return std::nullopt;
        }
        ComPtr<IDxcBlob> blob;
        if (HRESULT hr = compileResult->GetResult(&blob);
            FAILED(hr)) {
            RADRAY_ERR_LOG("dxc error, code={}", hr);
            return std::nullopt;
        }
        auto blobStart = std::bit_cast<byte const*>(blob->GetBufferPointer());
        vector<byte> blobData{blobStart, blobStart + blob->GetBufferSize()};
        vector<byte> reflData{};
        if (!isSpirv && !isStripRefl) {
            ComPtr<IDxcBlob> reflBlob;
            if (HRESULT hr = compileResult->GetOutput(DXC_OUT_REFLECTION, IID_PPV_ARGS(&reflBlob), nullptr);
                SUCCEEDED(hr)) {
                auto reflStart = std::bit_cast<byte const*>(reflBlob->GetBufferPointer());
                reflData = {reflStart, reflStart + reflBlob->GetBufferSize()};
            } else {
                RADRAY_ERR_LOG("dxc cannot get reflection, code={}", hr);
            }
        }
        return DxcOutput{
            .Data = std::move(blobData),
            .Refl = std::move(reflData),
            .Category = isSpirv ? ShaderBlobCategory::SPIRV : ShaderBlobCategory::DXIL};
    }

    std::optional<DxilReflection> GetDxilReflection(ShaderStage stage, std::span<const byte> refl) noexcept {
        DxcBuffer buf{refl.data(), refl.size(), 0};
        ComPtr<ID3D12ShaderReflection> sr;
        if (HRESULT hr = _utils->CreateReflection(&buf, IID_PPV_ARGS(&sr));
            FAILED(hr)) {
            RADRAY_ERR_LOG("dxc util cannot create ID3D12ShaderReflection, code={}", hr);
            return std::nullopt;
        }
        D3D12_SHADER_DESC shaderDesc{};
        if (HRESULT hr = sr->GetDesc(&shaderDesc);
            FAILED(hr)) {
            RADRAY_ERR_LOG("dxc util cannot get D3D12_SHADER_DESC, code={}", hr);
            return std::nullopt;
        }
        DxilReflection result{};
        result.CBuffers.reserve(shaderDesc.ConstantBuffers);
        for (UINT i = 0; i < shaderDesc.ConstantBuffers; i++) {
            ID3D12ShaderReflectionConstantBuffer* cb = sr->GetConstantBufferByIndex(i);
            D3D12_SHADER_BUFFER_DESC bufDesc;
            cb->GetDesc(&bufDesc);
            auto&& cbt = result.CBuffers.emplace_back(DxilReflection::CBuffer{});
            cbt.Name = bufDesc.Name;
            cbt.Size = bufDesc.Size;
            cbt.Vars.reserve(bufDesc.Variables);
            for (UINT j = 0; j < bufDesc.Variables; j++) {
                ID3D12ShaderReflectionVariable* v = cb->GetVariableByIndex(j);
                D3D12_SHADER_VARIABLE_DESC varDesc;
                v->GetDesc(&varDesc);
                auto&& vart = cbt.Vars.emplace_back(DxilReflection::Variable{});
                vart.Name = varDesc.Name;
                vart.Start = varDesc.StartOffset;
                vart.Size = varDesc.Size;
            }
        }
        result.Binds.reserve(shaderDesc.BoundResources);
        for (UINT i = 0; i < shaderDesc.BoundResources; i++) {
            D3D12_SHADER_INPUT_BIND_DESC bindDesc;
            if (HRESULT hr = sr->GetResourceBindingDesc(i, &bindDesc);
                FAILED(hr)) {
                RADRAY_ERR_LOG("dxc ID3D12ShaderReflection cannot get D3D12_SHADER_INPUT_BIND_DESC, code={}", hr);
                return std::nullopt;
            }
            auto&& br = result.Binds.emplace_back(DxilReflection::BindResource{});
            br.Name = bindDesc.Name;
            br.Type = ([](D3D_SHADER_INPUT_TYPE type, D3D_SRV_DIMENSION dim) noexcept -> ShaderResourceType {
                if (type == D3D_SHADER_INPUT_TYPE::D3D_SIT_UAV_RWTYPED && dim == D3D_SRV_DIMENSION_BUFFER) {
                    return ShaderResourceType::RWBuffer;
                }
                if (type == D3D_SHADER_INPUT_TYPE::D3D_SIT_TEXTURE && dim == D3D_SRV_DIMENSION_BUFFER) {
                    return ShaderResourceType::Buffer;
                }
                switch (type) {
                    case D3D_SIT_CBUFFER: return ShaderResourceType::CBuffer;
                    case D3D_SIT_TBUFFER: return ShaderResourceType::Buffer;
                    case D3D_SIT_TEXTURE: return ShaderResourceType::Texture;
                    case D3D_SIT_SAMPLER: return ShaderResourceType::Sampler;
                    case D3D_SIT_UAV_RWTYPED: return ShaderResourceType::RWTexture;
                    case D3D_SIT_STRUCTURED: return ShaderResourceType::Buffer;
                    case D3D_SIT_UAV_RWSTRUCTURED: return ShaderResourceType::RWBuffer;
                    case D3D_SIT_BYTEADDRESS: return ShaderResourceType::Buffer;
                    case D3D_SIT_UAV_RWBYTEADDRESS: return ShaderResourceType::RWBuffer;
                    case D3D_SIT_UAV_APPEND_STRUCTURED: return ShaderResourceType::RWBuffer;
                    case D3D_SIT_UAV_CONSUME_STRUCTURED: return ShaderResourceType::RWBuffer;
                    case D3D_SIT_UAV_RWSTRUCTURED_WITH_COUNTER: return ShaderResourceType::RWBuffer;
                    case D3D_SIT_RTACCELERATIONSTRUCTURE: return ShaderResourceType::RayTracing;
                    case D3D_SIT_UAV_FEEDBACKTEXTURE: return ShaderResourceType::RWTexture;
                }
                Unreachable();
            })(bindDesc.Type, bindDesc.Dimension);
            br.Dim = ([](D3D_SRV_DIMENSION dim) noexcept -> TextureViewDimension {
                switch (dim) {
                    case D3D_SRV_DIMENSION_UNKNOWN: return TextureViewDimension::UNKNOWN;
                    case D3D_SRV_DIMENSION_BUFFER: return TextureViewDimension::UNKNOWN;
                    case D3D_SRV_DIMENSION_TEXTURE1D: return TextureViewDimension::Dim1D;
                    case D3D_SRV_DIMENSION_TEXTURE1DARRAY: return TextureViewDimension::Dim1DArray;
                    case D3D_SRV_DIMENSION_TEXTURE2D: return TextureViewDimension::Dim2D;
                    case D3D_SRV_DIMENSION_TEXTURE2DARRAY: return TextureViewDimension::Dim2DArray;
                    case D3D_SRV_DIMENSION_TEXTURE2DMS: return TextureViewDimension::Dim2D;
                    case D3D_SRV_DIMENSION_TEXTURE2DMSARRAY: return TextureViewDimension::Dim2DArray;
                    case D3D_SRV_DIMENSION_TEXTURE3D: return TextureViewDimension::Dim2D;
                    case D3D_SRV_DIMENSION_TEXTURECUBE: return TextureViewDimension::Cube;
                    case D3D_SRV_DIMENSION_TEXTURECUBEARRAY: return TextureViewDimension::CubeArray;
                    case D3D_SRV_DIMENSION_BUFFEREX: return TextureViewDimension::UNKNOWN;
                }
                Unreachable();
            })(bindDesc.Dimension);
            br.Space = bindDesc.Space;
            br.BindPoint = bindDesc.BindPoint;
            br.BindCount = bindDesc.BindCount;
        }
        if (stage == ShaderStage::Vertex) {
            result.VertexInputs.reserve(shaderDesc.InputParameters);
            RADRAY_INFO_LOG("- inputs {}", shaderDesc.InputParameters);
            for (UINT i = 0; i < shaderDesc.InputParameters; i++) {
                D3D12_SIGNATURE_PARAMETER_DESC spDesc;
                if (HRESULT hr = sr->GetInputParameterDesc(i, &spDesc);
                    FAILED(hr)) {
                    RADRAY_ERR_LOG("dxc ID3D12ShaderReflection cannot get D3D12_SIGNATURE_PARAMETER_DESC, code={}", hr);
                    return std::nullopt;
                }
                auto&& vi = result.VertexInputs.emplace_back(DxilReflection::VertexInput{});
                vi.Semantic = spDesc.SemanticName;
                vi.SemanticIndex = spDesc.SemanticIndex;
                uint32_t comps = static_cast<uint32_t>(std::log2(spDesc.Mask));
                vi.Format = ([](D3D_REGISTER_COMPONENT_TYPE type, uint32_t coms) noexcept -> VertexFormat {
                    switch (type) {
                        case D3D_REGISTER_COMPONENT_UNKNOWN: return VertexFormat::UNKNOWN;
                        case D3D_REGISTER_COMPONENT_UINT32:
                            switch (coms) {
                                case 0: return VertexFormat::UINT32;
                                case 1: return VertexFormat::UINT32X2;
                                case 2: return VertexFormat::UINT32X3;
                                case 3: return VertexFormat::UINT32X4;
                                default: return VertexFormat::UNKNOWN;
                            }
                        case D3D_REGISTER_COMPONENT_SINT32:
                            switch (coms) {
                                case 0: return VertexFormat::SINT32;
                                case 1: return VertexFormat::SINT32X2;
                                case 2: return VertexFormat::SINT32X3;
                                case 3: return VertexFormat::SINT32X4;
                                default: return VertexFormat::UNKNOWN;
                            }
                        case D3D_REGISTER_COMPONENT_FLOAT32:
                            switch (coms) {
                                case 0: return VertexFormat::FLOAT32;
                                case 1: return VertexFormat::FLOAT32X2;
                                case 2: return VertexFormat::FLOAT32X3;
                                case 3: return VertexFormat::FLOAT32X4;
                                default: return VertexFormat::UNKNOWN;
                            }
                        default: return VertexFormat::UNKNOWN;
                    }
                })(spDesc.ComponentType, comps);
                RADRAY_INFO_LOG(" - attr {}{} {}", vi.Semantic, vi.SemanticIndex, vi.Format);
            }
        }
        if (stage == ShaderStage::Compute) {
            UINT x, y, z;
            sr->GetThreadGroupSize(&x, &y, &z);
            result.GroupSize = {x, y, z};
        }
        return result;
    }

public:
#ifdef RADRAY_ENABLE_MIMALLOC
    ComPtr<MiMallocAdapter> _mi;
#endif
    ComPtr<IDxcCompiler3> _dxc;
    ComPtr<IDxcUtils> _utils;
    ComPtr<IDxcIncludeHandler> _inc;
};

Nullable<shared_ptr<Dxc>> CreateDxc() noexcept {
    ComPtr<IDxcCompiler3> dxc;
#if RADRAY_ENABLE_MIMALLOC
    ComPtr<MiMallocAdapter> mi{new MiMallocAdapter{}};
    if (HRESULT hr = DxcCreateInstance2(mi.Get(), CLSID_DxcCompiler, IID_PPV_ARGS(&dxc));
        FAILED(hr)) {
        RADRAY_ERR_LOG("cannot create IDxcCompiler3, code={}", hr);
        return nullptr;
    }
#else
    if (HRESULT hr = DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&dxc));
        FAILED(hr)) {
        RADRAY_ERR_LOG("cannot create IDxcCompiler3, code={}", hr);
        return nullptr;
    }
#endif
    ComPtr<IDxcUtils> utils;
    if (HRESULT hr = DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&utils));
        FAILED(hr)) {
        RADRAY_ERR_LOG("cannot create IDxcUtils, code={}", hr);
        return nullptr;
    }
    ComPtr<IDxcIncludeHandler> incHandler;
    if (HRESULT hr = utils->CreateDefaultIncludeHandler(&incHandler);
        FAILED(hr)) {
        RADRAY_ERR_LOG("cannot create IDxcIncludeHandler, code={}", hr);
        return nullptr;
    }
    auto implPtr = make_unique<DxcImpl>(
#ifdef RADRAY_ENABLE_MIMALLOC
        std::move(mi),
#endif
        std::move(dxc),
        std::move(utils),
        std::move(incHandler));
    return make_shared<Dxc>(std::move(implPtr));
}

void Dxc::Destroy() noexcept {
    _impl.reset();
}

std::optional<DxcOutput> Dxc::Compile(std::string_view code, std::span<std::string_view> args) noexcept {
    return static_cast<DxcImpl*>(_impl.get())->Compile(code, args);
}

std::optional<DxcOutput> Dxc::Compile(
    std::string_view code,
    std::string_view entryPoint,
    ShaderStage stage,
    HlslShaderModel sm,
    bool isOptimize,
    std::span<std::string_view> defines,
    std::span<std::string_view> includes,
    bool isSpirv) noexcept {
    string smStr = ([stage, sm]() noexcept {
        using oss = std::basic_ostringstream<char, std::char_traits<char>, allocator<char>>;
        oss s{};
        switch (stage) {
            case ShaderStage::Vertex: s << "vs_"; break;
            case ShaderStage::Pixel: s << "ps_"; break;
            case ShaderStage::Compute: s << "cs_"; break;
            default: s << "??_"; break;
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
        string result = s.str();
        return result;
    })();
    vector<std::string_view> args{};
    if (isSpirv) {
        args.emplace_back("-spirv");
    }
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
    return static_cast<DxcImpl*>(_impl.get())->Compile(code, args);
}

std::optional<DxilReflection> Dxc::GetDxilReflection(ShaderStage stage, std::span<const byte> refl) noexcept {
    return static_cast<DxcImpl*>(_impl.get())->GetDxilReflection(stage, refl);
}

}  // namespace radray::render

#endif
