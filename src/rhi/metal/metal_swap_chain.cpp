#include "metal_swap_chain.h"

namespace radray::rhi::metal {

MetalSwapChain::MetalSwapChain(
    MTL::Device* device,
    uint64_t windowHandle,
    uint width,
    uint height,
    bool vsync,
    uint32_t backBufferCount)
    : layer(RadrayMetalCreateLayer(device, windowHandle, width, height, vsync, backBufferCount)) {}

MetalSwapChain::~MetalSwapChain() noexcept {
    if (layer != nullptr) {
        layer->release();
        layer = nullptr;
    }
    if (currentDrawable != nullptr) {
        currentDrawable->release();
        currentDrawable = nullptr;
    }
    currentBackBuffer = nullptr;
}

void MetalSwapChain::NextDrawable() {
    if (currentDrawable != nullptr) {
        currentDrawable->release();
    }
    currentDrawable = layer->nextDrawable()->retain();
    if (currentDrawable == nullptr) {
        RADRAY_WARN_LOG("Failed to acquire next drawable from swapchain.");
        currentBackBuffer = nullptr;
    } else {
        currentBackBuffer = currentDrawable->texture();
    }
}

}  // namespace radray::rhi::metal
