#include "metal_swap_chain.h"

#include "metal_device.h"

namespace radray::rhi::metal {

static NS::SharedPtr<MTL::Texture> GetTextureFromDrawable(CA::MetalDrawable* drawable) {
    auto tex = drawable->texture();
    return NS::RetainPtr(tex);
}

MetalSwapChainRenderTarget::MetalSwapChainRenderTarget(CA::MetalDrawable* drawable)
    : MetalTexture(GetTextureFromDrawable(drawable)),
      drawable(NS::TransferPtr(drawable)) {}

MetalSwapChain::MetalSwapChain(
    MetalDevice* device,
    uint64_t windowHandle,
    uint width,
    uint height,
    bool vsync,
    uint32_t backBufferCount) {
    auto ptr = RadrayMetalCreateLayer(device->device.get(), windowHandle, width, height, vsync, backBufferCount);
    layer = NS::TransferPtr(ptr);
    auto drawable = layer->nextDrawable();
    if (drawable == nullptr) {
        RADRAY_ERR_LOG("cannot get next drawable from MetalLayer");
    }
    nowRt = std::make_unique<MetalSwapChainRenderTarget>(drawable->retain());
}

void MetalSwapChain::NextDrawable() {
    auto drawable = layer->nextDrawable();
    if (drawable == nullptr) {
        RADRAY_ERR_LOG("cannot get next drawable from MetalLayer");
    }
    auto rt = nowRt.get();
    rt->~MetalSwapChainRenderTarget();
    new (std::launder(rt)) MetalSwapChainRenderTarget{drawable};
}

MetalSwapChain::~MetalSwapChain() noexcept = default;

}  // namespace radray::rhi::metal
