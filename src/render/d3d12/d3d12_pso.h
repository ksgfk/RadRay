#pragma once

#include <radray/render/pipeline_state.h>
#include "d3d12_helper.h"

namespace radray::render::d3d12 {

class GraphicsPsoD3D12 : public GraphicsPipelineState {
public:
    GraphicsPsoD3D12(
        ComPtr<ID3D12PipelineState> pso,
        radray::vector<uint32_t> arrayStrides,
        D3D12_PRIMITIVE_TOPOLOGY topo) noexcept
        : _pso(std::move(pso)),
          _arrayStrides(std::move(arrayStrides)),
          _topo(topo) {}
    ~GraphicsPsoD3D12() noexcept override = default;

    bool IsValid() const noexcept override;
    void Destroy() noexcept override;

public:
    ComPtr<ID3D12PipelineState> _pso;
    radray::vector<uint32_t> _arrayStrides;
    D3D12_PRIMITIVE_TOPOLOGY _topo;
};

}  // namespace radray::render::d3d12
