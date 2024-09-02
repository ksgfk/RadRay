#include "metal_swapchain.h"

namespace radray::rhi::metal {

SwapChain::SwapChain(
    MTL::Device* device,
    size_t windowHandle,
    uint width,
    uint height,
    uint backBufferCount,
    MTL::PixelFormat format,
    bool enableSync)
    : layer(RadrayMetalCreateLayer(
          device,
          windowHandle,
          width,
          height,
          backBufferCount,
          format == MTL::PixelFormatRGBA16Float,
          enableSync)) {
    if (format != MTL::PixelFormatRGBA16Float && format != MTL::PixelFormatBGRA8Unorm) {
        RADRAY_WARN_LOG("CAMetalLayer only support format RGBA16Float or BGRA8Unorm");
    }
}
SwapChain::~SwapChain() noexcept {
    layer->release();
}

}  // namespace radray::rhi::metal
