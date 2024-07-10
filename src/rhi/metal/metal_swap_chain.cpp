#include "metal_swap_chain.h"

namespace radray::rhi::metal {

MetalSwapChain::MetalSwapChain(
    MTL::Device* device,
    uint64_t windowHandle,
    uint width,
    uint height,
    bool vsync,
    uint32_t backBufferCount)
    : layer(RadrayMetalCreateLayer(device, windowHandle, width, height, vsync, backBufferCount)),
      backBuffer(std::make_unique<MetalTexture>(
          device,
          MTL::PixelFormat::PixelFormatRGBA8Unorm,
          MTL::TextureType::TextureType2D,
          width, height,
          1,
          1)) {}

MetalSwapChain::~MetalSwapChain() noexcept {
    if (layer != nullptr) {
        layer->release();
        layer = nullptr;
    }
}

}  // namespace radray::rhi::metal
