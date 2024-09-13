#include "metal_texture.h"

namespace radray::rhi::metal {

Texture::Texture(
    MTL::Device* device,
    MTL::TextureDescriptor* desc)
    : texture(device->newTexture(desc)) {}

Texture::~Texture() noexcept {
    if (texture != nullptr) {
        texture->release();
        texture = nullptr;
    }
}

MetalDrawableTexture::MetalDrawableTexture(CA::MetalDrawable* drawable)
    : Texture(drawable->texture()),
      drawable(drawable->retain()) {}

MetalDrawableTexture::~MetalDrawableTexture() noexcept {
    texture = nullptr;
    if (drawable != nullptr) {
        drawable->release();
        drawable = nullptr;
    }
}

TextureView::TextureView(Texture* raw, MTL::PixelFormat format)
    : raw(raw),
      texture(raw->texture->newTextureView(format)) {}

TextureView::~TextureView() noexcept {
    texture->release();
}

}  // namespace radray::rhi::metal
