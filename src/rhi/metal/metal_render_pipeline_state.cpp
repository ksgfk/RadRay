#include "metal_render_pipeline_state.h"

namespace radray::rhi::metal {

static MTL::RenderPipelineState* CreateRenderPsoImpl(
    MTL::Device* device,
    MTL::RenderPipelineDescriptor* desc) {
    NS::Error* err{nullptr};
    MTL::RenderPipelineState* result = device->newRenderPipelineState(desc, &err);
    if (err != nullptr) {
        RADRAY_ERR_LOG("create render pipeline state fail. {}", err->localizedDescription()->utf8String());
        return nullptr;
    }
    return result;
}

MetalRenderPipelineState::MetalRenderPipelineState(
    MTL::Device* device,
    MTL::RenderPipelineDescriptor* desc) : pso(CreateRenderPsoImpl(device, desc)) {}

MetalRenderPipelineState::~MetalRenderPipelineState() noexcept {
    if (pso != nullptr) {
        pso->release();
        pso = nullptr;
    }
}

bool MetalRenderPipelineState::IsValid() const noexcept { return pso != nullptr; }

}  // namespace radray::rhi::metal
