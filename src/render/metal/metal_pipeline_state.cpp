#include "metal_pipeline_state.h"

namespace radray::render::metal {

void RenderPipelineStateMetal::Destroy() noexcept {
    _pso.reset();
    _depthStencil.reset();
}

}  // namespace radray::render::metal
