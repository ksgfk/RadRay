#pragma once

#include "metal_helper.h"
#include "metal_texture.h"

extern "C" CA::MetalLayer* RadrayMetalCreateLayer(
    MTL::Device* device,
    uint64_t windowHandle,
    uint32_t width,
    uint32_t height,
    bool vsync,
    uint32_t backBufferCount) noexcept;

namespace radray::rhi::metal {

class MetalSwapChain {
public:
    MetalSwapChain(
        MTL::Device* device,
        uint64_t windowHandle,
        uint width,
        uint height,
        bool vsync,
        uint32_t backBufferCount);
    MetalSwapChain(const MetalSwapChain&) = delete;
    MetalSwapChain(MetalSwapChain&&) = delete;
    MetalSwapChain& operator=(const MetalSwapChain&) = delete;
    MetalSwapChain& operator=(MetalSwapChain&&) = delete;
    ~MetalSwapChain() noexcept;

public:
    CA::MetalLayer* layer;
    std::unique_ptr<MetalTexture> backBuffer;
};

}  // namespace radray::rhi::metal
