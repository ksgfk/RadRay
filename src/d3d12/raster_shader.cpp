#include <radray/d3d12/raster_shader.h>

#include <cstring>
#include <xxhash.h>
#include <radray/d3d12/device.h>

namespace radray::d3d12 {

size_t RasterPipelineStateInfoHash::operator()(const RasterPipelineStateInfo& v) const noexcept {
    static_assert(sizeof(size_t) == sizeof(XXH64_hash_t) || sizeof(size_t) == sizeof(XXH32_hash_t), "no way. 16bit or 128bit+ platform?");
    const RasterPipelineStateInfo* ptr = &v;
    if constexpr (sizeof(size_t) == sizeof(XXH64_hash_t)) {
        XXH64_hash_t hash = XXH64(ptr, sizeof(std::remove_pointer_t<decltype(ptr)>), 0);
        return hash;
    } else if constexpr (sizeof(size_t) == sizeof(XXH32_hash_t)) {
        XXH32_hash_t hash = XXH32(ptr, sizeof(std::remove_pointer_t<decltype(ptr)>), 0);
        return hash;
    } else {
        return 0;
    }
}

bool RasterPipelineStateInfoEqual::operator()(const RasterPipelineStateInfo& l, const RasterPipelineStateInfo& r) const noexcept {
    const RasterPipelineStateInfo* pl = &l;
    const RasterPipelineStateInfo* pr = &l;
    int i = std::memcmp(pl, pr, sizeof(RasterPipelineStateInfo));
    return i == 0;
}

RasterShader::RasterShader(Device* device) noexcept : Shader(device) {}

ComPtr<ID3D12PipelineState> RasterShader::GetOrCreatePso(const RasterPipelineStateInfo& info) noexcept {
    auto [iter, isNew] = psoCache.try_emplace(info, ComPtr<ID3D12PipelineState>{nullptr});
    if (isNew) {
        D3D12_INPUT_ELEMENT_DESC inputLayout[RasterPipelineStateInfo::MaxInputLayout];
        for (size_t i = 0; i < ArrayLength(inputLayout); i++) {
            const InputElementInfo& t = info.InputLayouts[i];
            inputLayout[i] = {
                .SemanticName = EnumSemanticToString(t.Semantic),
                .SemanticIndex = t.SemanticIndex,
                .Format = t.Format,
                .InputSlot = t.InputSlot,
                .AlignedByteOffset = t.AlignedByteOffset,
                .InputSlotClass = t.InputSlotClass,
                .InstanceDataStepRate = t.InstanceDataStepRate};
        }
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = rootSig.Get();
        desc.VS = {vsBinary.data(), vsBinary.size()};
        desc.PS = {psBinary.data(), psBinary.size()};
        desc.StreamOutput = {};
        desc.BlendState = info.BlendState;
        desc.SampleMask = info.SampleMask;
        desc.RasterizerState = info.RasterizerState;
        desc.DepthStencilState = info.DepthStencilState;
        desc.InputLayout = {inputLayout, info.NumInputs};
        desc.IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
        desc.PrimitiveTopologyType = info.PrimitiveTopologyType;
        desc.NumRenderTargets = info.NumRenderTargets;
        std::copy(ArrayFirst(info.RtvFormats), ArrayLast(info.RtvFormats), desc.RTVFormats);
        desc.DSVFormat = info.DSVFormat;
        desc.SampleDesc = info.SampleDesc;
        ComPtr<ID3D12PipelineState> pso;
        ThrowIfFailed(device->device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(pso.GetAddressOf())));
        iter->second = pso;
    }
    return ComPtr<ID3D12PipelineState>{iter->second};
}

const char* EnumSemanticToString(InputElementSemantic e) noexcept {
    switch (e) {
        case InputElementSemantic::POSITION: return "POSITION";
        case InputElementSemantic::NORMAL: return "NORMAL";
        case InputElementSemantic::TEXCOORD: return "TEXCOORD";
        case InputElementSemantic::TANGENT: return "TANGENT";
        case InputElementSemantic::COLOR: return "COLOR";
        case InputElementSemantic::PSIZE: return "PSIZE";
        case InputElementSemantic::BINORMAL: return "BINORMAL";
        case InputElementSemantic::BLENDINDICES: return "BLENDINDICES";
        case InputElementSemantic::BLENDWEIGHT: return "BLENDWEIGHT";
        case InputElementSemantic::POSITIONT: return "POSITIONT";
        case InputElementSemantic::FOG: return "FOG";
        case InputElementSemantic::TESSFACTOR: return "TESSFACTOR";
        default: return "UNKNOWN";
    }
}

}  // namespace radray::d3d12
