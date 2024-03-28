#include <radray/d3d12/raster_shader.h>

#include <cstring>
#include <xxhash.h>
#include <radray/vertex_data.h>
#include <radray/d3d12/device.h>
#include <radray/d3d12/shader_compiler.h>

namespace radray::d3d12 {

void RasterPipelineStateInfo::InitDefault() noexcept {
    BlendState = CD3DX12_BLEND_DESC{CD3DX12_DEFAULT{}};
    SampleMask = DefaultSampleMask{};
    RasterizerState = CD3DX12_RASTERIZER_DESC{CD3DX12_DEFAULT{}};
    DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC{CD3DX12_DEFAULT{}};
    PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE::D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    NumRenderTargets = 1;
    RtvFormats[0] = DXGI_FORMAT::DXGI_FORMAT_R8G8B8A8_UNORM;
    DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    SampleDesc = DefaultSampleDesc{};
}

void RasterPipelineStateInfo::InitInputElements(const std::vector<RasterInputElementDefine>& targetShaderInput, const VertexData* vertexData, uint32 slot) noexcept {
    auto&& vertexLayouts = vertexData->layouts;
    NumInputs = 0;
    for (auto&& i : targetShaderInput) {
        uint32 targetSize = DxgiFormatByteSize(i.Format);
        bool anyFind = false;
        for (auto&& j : vertexLayouts) {
            if (NumInputs >= MaxInputLayout) {
                break;
            }
            if (i.Semantic == j.Semantic && i.SemanticIndex == j.SemanticIndex && targetSize == j.Size) {
                InputLayouts[NumInputs] = RasterInputElementInfo{
                    j.Semantic,
                    j.SemanticIndex,
                    i.Format,
                    slot,
                    j.Offset,
                    D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
                    0};
                NumInputs++;
                anyFind = true;
                break;
            }
        }
        if (!anyFind) {
            RADRAY_ABORT("cannot find vertex semantic {} {}", EnumSemanticToString(i.Semantic), i.SemanticIndex);
        }
    }
}

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
            const RasterInputElementInfo& t = info.InputLayouts[i];
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

void RasterShader::Setup(const RasterShaderCompileResult* result) {
    {
        using BindTable = std::unordered_map<std::string, ShaderProperty>;
        BindTable bindTable{};
        auto insertToBindTable = [&bindTable](ID3D12ShaderReflection* refl) {
            D3D12_SHADER_DESC desc;
            refl->GetDesc(&desc);
            for (UINT i = 0; i < desc.BoundResources; i++) {
                D3D12_SHADER_INPUT_BIND_DESC sb;
                refl->GetResourceBindingDesc(i, &sb);
                ShaderProperty p{
                    .type = ([](D3D_SHADER_INPUT_TYPE type, uint32 cnt) {
                        switch (type) {
                            // cbuffer MyConstantBuffer : register(b0) or ConstantBuffer<MyStruct> cbuffers[] : register(b0, space1)
                            case D3D_SIT_CBUFFER: return cnt == 1 ? ShaderVariableType::ConstantBuffer : ShaderVariableType::CBVBufferHeap;
                            // Texture2D<float4> textures[] : register(t0, space1)
                            case D3D_SIT_TEXTURE: return ShaderVariableType::SRVTextureHeap;
                            // SamplerState samplers[16] : register(s0)
                            case D3D_SIT_SAMPLER: return ShaderVariableType::SamplerHeap;
                            // RWTexture2D<float4> textures[] : register(u0, space1)
                            case D3D_SIT_UAV_RWTYPED: return ShaderVariableType::UAVTextureHeap;
                            // StructuredBuffer<MyStruct> : register(t0) or StructuredBuffer<MyStruct> buffers[] : register(t0, space1)
                            case D3D_SIT_STRUCTURED: return cnt == 1 ? ShaderVariableType::StructuredBuffer : ShaderVariableType::SRVBufferHeap;
                            // RWStructuredBuffer<MyStruct> : register(u0) or RWStructuredBuffer<MyStruct> buffers[] : register(u0, space1)
                            case D3D_SIT_UAV_RWSTRUCTURED: return cnt == 1 ? ShaderVariableType::RWStructuredBuffer : ShaderVariableType::UAVBufferHeap;
                            // ByteAddressBuffer bytes : register(t0) or ByteAddressBuffer bytes[] : register(t0, space1)
                            case D3D_SIT_BYTEADDRESS: return cnt == 1 ? ShaderVariableType::StructuredBuffer : ShaderVariableType::SRVBufferHeap;
                            // RWByteAddressBuffer : register(u0, space1)
                            case D3D_SIT_UAV_RWBYTEADDRESS: return cnt == 1 ? ShaderVariableType::RWStructuredBuffer : ShaderVariableType::UAVBufferHeap;
                            case D3D_SIT_UAV_APPEND_STRUCTURED: return cnt == 1 ? ShaderVariableType::RWStructuredBuffer : ShaderVariableType::UAVBufferHeap;
                            case D3D_SIT_UAV_CONSUME_STRUCTURED: return cnt == 1 ? ShaderVariableType::RWStructuredBuffer : ShaderVariableType::UAVBufferHeap;
                            default: RADRAY_ABORT("unknown D3D_SHADER_INPUT_TYPE {}", type); return ShaderVariableType::ConstantBuffer;
                        }
                    })(sb.Type, sb.BindCount),
                    .spaceIndex = sb.Space,
                    .registerIndex = sb.BindPoint,
                    .arraySize = sb.BindCount};
                std::string nm{sb.Name};
                auto&& [iter, isInsert] = bindTable.try_emplace(nm, p);
                if (!isInsert) {
                    auto&& inserted = iter->second;
                    if (std::memcmp(&inserted, &p, sizeof(ShaderProperty)) != 0) {
                        RADRAY_ABORT(
                            "shader property with the same name but different structures. name={}\n"
                            "exist: type={}, registerIndex={}, arraySize={}, spaceIndex={}\n"
                            "diff:  type={}, registerIndex={}, arraySize={}, spaceIndex={}",
                            nm,
                            inserted.type, inserted.registerIndex, inserted.arraySize, inserted.spaceIndex,
                            p.type, p.registerIndex, p.arraySize, p.spaceIndex);
                    }
                }
            }
        };
        insertToBindTable(result->vs.refl.Get());
        insertToBindTable(result->ps.refl.Get());
        std::vector<CD3DX12_ROOT_PARAMETER> allParameter;
        std::vector<CD3DX12_DESCRIPTOR_RANGE> allRange;
        for (auto&& property : bindTable) {
            auto&& var = property.second;
            switch (var.type) {
                case ShaderVariableType::UAVBufferHeap:
                case ShaderVariableType::UAVTextureHeap:
                case ShaderVariableType::CBVBufferHeap:
                case ShaderVariableType::SamplerHeap:
                case ShaderVariableType::SRVBufferHeap:
                case ShaderVariableType::SRVTextureHeap: {
                    allRange.emplace_back();
                    break;
                }
                default:
                    break;
            }
        }
        size_t offset = 0;
        for (auto&& property : bindTable) {
            auto&& var = property.second;
            switch (var.type) {
                case ShaderVariableType::SRVTextureHeap:
                case ShaderVariableType::SRVBufferHeap: {
                    CD3DX12_DESCRIPTOR_RANGE& range = allRange[offset];
                    offset++;
                    range.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, var.arraySize, var.registerIndex, var.spaceIndex);
                    allParameter.emplace_back().InitAsDescriptorTable(1, &range);
                    break;
                }
                case ShaderVariableType::CBVBufferHeap: {
                    CD3DX12_DESCRIPTOR_RANGE& range = allRange[offset];
                    offset++;
                    range.Init(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, var.arraySize, var.registerIndex, var.spaceIndex);
                    allParameter.emplace_back().InitAsDescriptorTable(1, &range);
                    break;
                }
                case ShaderVariableType::SamplerHeap: {
                    CD3DX12_DESCRIPTOR_RANGE& range = allRange[offset];
                    offset++;
                    range.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, var.arraySize, var.registerIndex, var.spaceIndex);
                    allParameter.emplace_back().InitAsDescriptorTable(1, &range);
                    break;
                }
                case ShaderVariableType::UAVTextureHeap:
                case ShaderVariableType::UAVBufferHeap: {
                    CD3DX12_DESCRIPTOR_RANGE& range = allRange[offset];
                    offset++;
                    range.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, var.arraySize, var.registerIndex, var.spaceIndex);
                    allParameter.emplace_back().InitAsDescriptorTable(1, &range);
                    break;
                }
                case ShaderVariableType::ConstantBuffer:
                    allParameter.emplace_back().InitAsConstantBufferView(var.registerIndex, var.spaceIndex);
                    break;
                case ShaderVariableType::StructuredBuffer:
                    allParameter.emplace_back().InitAsShaderResourceView(var.registerIndex, var.spaceIndex);
                    break;
                case ShaderVariableType::RWStructuredBuffer:
                    allParameter.emplace_back().InitAsUnorderedAccessView(var.registerIndex, var.spaceIndex);
                    break;
                default: RADRAY_ABORT("unknown ShaderVariableType {}", var.type); break;
            }
        }
        CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rootSigDesc(
            allParameter.size(), allParameter.data(),
            0, nullptr,
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);
        ComPtr<ID3DBlob> serializedRootSig;
        ComPtr<ID3DBlob> errorBlob;
        ThrowIfFailed(D3D12SerializeVersionedRootSignature(
            &rootSigDesc,
            serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf()));
        if (errorBlob && errorBlob->GetBufferSize() > 0) {
            RADRAY_ABORT("Serialize root signature error: {}", std::string_view(reinterpret_cast<char const*>(errorBlob->GetBufferPointer()), errorBlob->GetBufferSize()));
        }
        ThrowIfFailed(device->device->CreateRootSignature(
            0,
            serializedRootSig->GetBufferPointer(),
            serializedRootSig->GetBufferSize(),
            IID_PPV_ARGS(rootSig.GetAddressOf())));
        properties.clear();
        nameToIndex.clear();
        psoCache.clear();
        {
            auto&& sasm = result->vs.data.Get();
            vsBinary.resize(sasm->GetBufferSize());
            std::memcpy(vsBinary.data(), sasm->GetBufferPointer(), sasm->GetBufferSize());
        }
        {
            auto&& sasm = result->ps.data.Get();
            psBinary.resize(sasm->GetBufferSize());
            std::memcpy(psBinary.data(), sasm->GetBufferPointer(), sasm->GetBufferSize());
        }
        for (auto&& property : bindTable) {
            auto&& var = property.second;
            size_t i = properties.size();
            properties.emplace_back(var);
            nameToIndex.try_emplace(std::string{property.first}, i);
        }
    }
    {
        inputDefines.clear();
        ID3D12ShaderReflection* vsRefl = result->vs.refl.Get();
        D3D12_SHADER_DESC desc;
        vsRefl->GetDesc(&desc);
        for (uint32 i = 0; i < desc.InputParameters; i++) {
            D3D12_SIGNATURE_PARAMETER_DESC input;
            vsRefl->GetInputParameterDesc(i, &input);
            RasterInputElementDefine inputdef{
                .Semantic = StringToEnumSemantic(std::string{input.SemanticName}),
                .SemanticIndex = input.SemanticIndex,
                .Format = ([&input]() {
                    switch (input.ComponentType) {
                        case D3D_REGISTER_COMPONENT_UINT32: {
                            switch (input.Mask) {
                                case 0b0001: return DXGI_FORMAT_R32_UINT;
                                case 0b0011: return DXGI_FORMAT_R32G32_UINT;
                                case 0b0111: return DXGI_FORMAT_R32G32B32_UINT;
                                case 0b1111: return DXGI_FORMAT_R32G32B32A32_UINT;
                                default: return DXGI_FORMAT_UNKNOWN;
                            }
                        }
                        case D3D_REGISTER_COMPONENT_SINT32: {
                            switch (input.Mask) {
                                case 0b0001: return DXGI_FORMAT_R32_SINT;
                                case 0b0011: return DXGI_FORMAT_R32G32_SINT;
                                case 0b0111: return DXGI_FORMAT_R32G32B32_SINT;
                                case 0b1111: return DXGI_FORMAT_R32G32B32A32_SINT;
                                default: return DXGI_FORMAT_UNKNOWN;
                            }
                        }
                        case D3D_REGISTER_COMPONENT_FLOAT32: {
                            switch (input.Mask) {
                                case 0b0001: return DXGI_FORMAT_R32_FLOAT;
                                case 0b0011: return DXGI_FORMAT_R32G32_FLOAT;
                                case 0b0111: return DXGI_FORMAT_R32G32B32_FLOAT;
                                case 0b1111: return DXGI_FORMAT_R32G32B32A32_FLOAT;
                                default: return DXGI_FORMAT_UNKNOWN;
                            }
                        }
                        default: return DXGI_FORMAT_UNKNOWN;
                    }
                })()};
            if (inputdef.Format == DXGI_FORMAT_UNKNOWN) {
                RADRAY_ABORT("unknown format {} {}", input.ComponentType, input.Mask);
            }
            inputDefines.emplace_back(inputdef);
        }
    }
}

}  // namespace radray::d3d12
