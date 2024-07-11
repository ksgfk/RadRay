#pragma once

#include "metal_helper.h"

namespace radray::rhi::metal {

class MetalTexture {
public:
    MetalTexture(MTL::Texture* ptr, bool isRetain);
    MetalTexture(
        MTL::Device* device,
        MTL::PixelFormat foramt,
        MTL::TextureType type,
        uint width, uint height,
        uint depth,
        uint mipmap,
        MTL::TextureUsage usage = MTL::TextureUsageShaderRead | MTL::TextureUsageShaderWrite);
    MetalTexture(const MetalTexture&) = delete;
    MetalTexture(MetalTexture&&) = delete;
    MetalTexture& operator=(const MetalTexture&) = delete;
    MetalTexture& operator=(MetalTexture&&) = delete;
    virtual ~MetalTexture() noexcept;

public:
    MTL::Texture* texture;
};

}  // namespace radray::rhi::metal
