#include "metal_swap_chain.h"

#include "metal_device.h"

namespace radray::rhi::metal {

MetalSwapChain::MetalSwapChain(
    MetalDevice* device,
    uint64_t windowHandle,
    uint width,
    uint height,
    bool vsync,
    uint32_t backBufferCount) {
    auto ptr = RadrayMetalCreateLayer(device->device.get(), windowHandle, width, height, vsync, backBufferCount);
    layer = NS::TransferPtr(ptr);
}

MetalSwapChain::~MetalSwapChain() noexcept = default;

}  // namespace radray::rhi::metal
