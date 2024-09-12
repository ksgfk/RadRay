#pragma once

#include "metal_helper.h"

namespace radray::rhi::metal {

class Texture {
public:
    Texture(
        MTL::Device* device,
        MTL::TextureDescriptor* desc);
    ~Texture() noexcept;

public:
    MTL::Texture* texture;
};

class TextureView {
public:
    TextureView(Texture* raw, MTL::PixelFormat format);
    ~TextureView() noexcept;

public:
    Texture* raw;
    MTL::Texture* texture;
};

}  // namespace radray::rhi::metal
