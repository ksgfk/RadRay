#pragma once

#include <radray/d3d12/shader.h>

namespace radray::d3d12 {

struct RasterPipelineStateInfo {
    D3D12_BLEND_DESC BlendState;
    D3D12_RASTERIZER_DESC RasterizerState;
    D3D12_DEPTH_STENCIL_DESC DepthStencilState;
    D3D12_PRIMITIVE_TOPOLOGY_TYPE PrimitiveTopologyType;
    uint32 NumRenderTargets;
    DXGI_FORMAT RtvFormats[8];
    DXGI_FORMAT DSVFormat;
    DXGI_SAMPLE_DESC SampleDesc;
};

static_assert(std::is_trivially_copyable_v<RasterPipelineStateInfo>, "raster pso info must be trivially copyable");
static_assert(std::is_standard_layout_v<RasterPipelineStateInfo>, "raster pso info must be standard layout");

class RasterShader : public Shader {
public:
    ~RasterShader() noexcept override = default;

public:
    std::vector<D3D12_INPUT_ELEMENT_DESC> elements;
    std::vector<uint8> vertexBinary;
    std::vector<uint8> pixelBinary;
};

}  // namespace radray::d3d12
