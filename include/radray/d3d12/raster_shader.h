#pragma once

#include <unordered_map>
#include <vector>
#include <radray/vertex_data.h>
#include <radray/d3d12/shader.h>

namespace radray::d3d12 {

struct RasterShaderCompileResult;

struct InputElementInfo {
    InputElementSemantic Semantic;
    uint32 SemanticIndex;
    DXGI_FORMAT Format;
    uint32 InputSlot;
    uint32 AlignedByteOffset;
    D3D12_INPUT_CLASSIFICATION InputSlotClass;
    uint32 InstanceDataStepRate;
};

struct RasterPipelineStateInfo {
    static constexpr size_t MaxInputLayout = 16;

    D3D12_BLEND_DESC BlendState;
    uint32 SampleMask;
    D3D12_RASTERIZER_DESC RasterizerState;
    D3D12_DEPTH_STENCIL_DESC DepthStencilState;
    D3D12_PRIMITIVE_TOPOLOGY_TYPE PrimitiveTopologyType;
    uint32 NumRenderTargets;
    DXGI_FORMAT RtvFormats[8];
    DXGI_FORMAT DSVFormat;
    DXGI_SAMPLE_DESC SampleDesc;
    uint32 NumInputs;
    InputElementInfo InputLayouts[MaxInputLayout];
};

static_assert(std::is_trivially_copyable_v<RasterPipelineStateInfo>, "raster pso info must be trivially copyable");
static_assert(std::is_standard_layout_v<RasterPipelineStateInfo>, "raster pso info must be standard layout");

struct RasterPipelineStateInfoHash {
    size_t operator()(const RasterPipelineStateInfo& v) const noexcept;
};

struct RasterPipelineStateInfoEqual {
    bool operator()(const RasterPipelineStateInfo& l, const RasterPipelineStateInfo& r) const noexcept;
};

using PsoMap = std::unordered_map<RasterPipelineStateInfo, ComPtr<ID3D12PipelineState>, RasterPipelineStateInfoHash, RasterPipelineStateInfoEqual>;

class RasterShader : public Shader {
public:
    RasterShader(Device* device) noexcept;
    ~RasterShader() noexcept override = default;

    ComPtr<ID3D12PipelineState> GetOrCreatePso(const RasterPipelineStateInfo&) noexcept;
    void Setup(const RasterShaderCompileResult* result);

public:
    ComPtr<ID3D12RootSignature> rootSig;
    std::vector<uint8> vsBinary;
    std::vector<uint8> psBinary;
    PsoMap psoCache;
};

}  // namespace radray::d3d12
