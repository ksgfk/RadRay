#pragma once

#include "metal_helper.h"

namespace radray::rhi::metal {

class MetalDevice;

class MetalTexture {
public:
    explicit MetalTexture(NS::SharedPtr<MTL::Texture> ptr);
    MetalTexture(
        MetalDevice* device,
        MTL::PixelFormat foramt,
        uint dimension,
        uint width, uint height,
        uint depth,
        uint mipmap);

public:
    NS::SharedPtr<MTL::Texture> texture;
};

}  // namespace radray::rhi::metal
