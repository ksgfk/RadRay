#pragma once

#include "metal_helper.h"

namespace radray::rhi::metal {

class Texture {
public:
    explicit Texture(MTL::Texture* texture) noexcept : texture(texture) {}
    Texture(
        MTL::Device* device,
        MTL::TextureDescriptor* desc);
    virtual ~Texture() noexcept;

public:
    MTL::Texture* texture;
};

class MetalDrawableTexture : public Texture {
public:
    explicit MetalDrawableTexture(CA::MetalDrawable* drawable);
    ~MetalDrawableTexture() noexcept override;

public:
    CA::MetalDrawable* drawable;
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
