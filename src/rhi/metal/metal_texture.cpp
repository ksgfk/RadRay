#include "metal_texture.h"

namespace radray::rhi::metal {

Texture::Texture(
    MTL::Device* device,
    MTL::TextureDescriptor* desc)
    : texture(device->newTexture(desc)) {}

Texture::~Texture() noexcept {
    texture->release();
}

TextureView::TextureView(Texture* raw, MTL::PixelFormat format)
    : raw(raw),
      texture(raw->texture->newTextureView(format)) {}

TextureView::~TextureView() noexcept {
    texture->release();
}

}  // namespace radray::rhi::metal
