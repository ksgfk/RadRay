#pragma once

#include "metal_helper.h"

namespace radray::rhi::metal {

class SwapChain {
public:
    SwapChain(
        MTL::Device* device,
        MTL::CommandQueue* queue,
        size_t windowHandle,
        uint width,
        uint height,
        uint backBufferCount,
        MTL::PixelFormat format,
        bool enableSync);
    ~SwapChain() noexcept;

    void AcquireNextDrawable();

public:
    MTL::CommandQueue* queue;
    CA::MetalLayer* layer;
    CA::MetalDrawable* currentDrawable{nullptr};
};

}  // namespace radray::rhi::metal

extern "C" CA::MetalLayer* RadrayMetalCreateLayer(
    MTL::Device* device,
    size_t windowHandle,
    uint32_t width,
    uint32_t height,
    uint32_t backBufferCount,
    bool isHdr,
    bool enableSync);
