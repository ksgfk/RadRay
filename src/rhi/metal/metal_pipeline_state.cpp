#include "metal_pipeline_state.h"

namespace radray::rhi::metal {

RenderPipelineState::RenderPipelineState(MTL::Device* device, const MTL::RenderPipelineDescriptor* desc) {
    NS::Error* err{nullptr};
    pso = device->newRenderPipelineState(desc, &err);
    if (err != nullptr) {
        err->autorelease();
        RADRAY_MTL_THROW("cannot create render pipeline state, {}", err->localizedDescription()->utf8String());
    }
}

RenderPipelineState::~RenderPipelineState() noexcept {
    if (pso != nullptr) {
        pso->release();
        pso = nullptr;
    }
}

}  // namespace radray::rhi::metal
