#pragma once

#include "metal_helper.h"

extern "C" CA::MetalLayer* RadrayMetalCreateLayer(
    MTL::Device* device,
    uint64_t windowHandle,
    uint32_t width,
    uint32_t height,
    bool vsync,
    uint32_t backBufferCount) noexcept;

namespace radray::rhi::metal {

class MetalDevice;

class MetalSwapChain {
public:
    MetalSwapChain(
        MetalDevice* device,
        uint64_t windowHandle,
        uint width,
        uint height,
        bool vsync,
        uint32_t backBufferCount);
    ~MetalSwapChain() noexcept;

public:
    NS::SharedPtr<CA::MetalLayer> layer;
};

}  // namespace radray::rhi::metal
