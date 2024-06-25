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

class MetalDevice;

class MetalSwapChainRenderTarget : public MetalTexture {
public:
    explicit MetalSwapChainRenderTarget(CA::MetalDrawable* drawable);
    ~MetalTexture() noexcept override = default;

public:
    NS::SharedPtr<CA::MetalDrawable> drawable;
};

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
    std::unique_ptr<MetalSwapChainRenderTarget> nowRt;
};

}  // namespace radray::rhi::metal
