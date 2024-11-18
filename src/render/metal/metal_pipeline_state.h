#pragma once

#include <radray/render/pipeline_state.h>
#include "metal_helper.h"

namespace radray::render::metal {

class RenderPipelineStateMetal : public GraphicsPipelineState {
public:
    RenderPipelineStateMetal(
        NS::SharedPtr<MTL::RenderPipelineState> pso,
        MTL::PrimitiveType primType,
        MTL::TriangleFillMode fill,
        MTL::Winding winding,
        MTL::CullMode cull,
        MTL::DepthClipMode depthClip,
        NS::SharedPtr<MTL::DepthStencilState> depthStencil,
        DepthBiasState depthBias) noexcept
        : _pso(std::move(pso)),
          _primType(primType),
          _fill(fill),
          _winding(winding),
          _cull(cull),
          _depthClip(depthClip),
          _depthStencil(std::move(depthStencil)),
          _depthBias(depthBias) {}
    ~RenderPipelineStateMetal() noexcept override = default;

    bool IsValid() const noexcept override { return _pso.get() != nullptr; }
    void Destroy() noexcept override;

public:
    NS::SharedPtr<MTL::RenderPipelineState> _pso;
    MTL::PrimitiveType _primType;
    MTL::TriangleFillMode _fill;
    MTL::Winding _winding;
    MTL::CullMode _cull;
    MTL::DepthClipMode _depthClip;
    NS::SharedPtr<MTL::DepthStencilState> _depthStencil;
    DepthBiasState _depthBias;
};

}  // namespace radray::render::metal
