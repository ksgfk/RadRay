#include "metal_pipeline_state.h"

namespace radray::render::metal {

RenderPipelineStateMetal::RenderPipelineStateMetal(
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

void RenderPipelineStateMetal::Destroy() noexcept {
    _pso.reset();
    _depthStencil.reset();
}

}  // namespace radray::render::metal
