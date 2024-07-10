#include "metal_texture.h"

namespace radray::rhi::metal {

static MTL::Texture* CreateTextureImpl(
    MTL::Device* device,
    MTL::PixelFormat foramt,
    MTL::TextureType type,
    uint width, uint height,
    uint depth,
    uint mipmap) {
    ScopedAutoreleasePool arp_{};
    MTL::TextureDescriptor* desc = MTL::TextureDescriptor::alloc()->init()->autorelease();
    desc->setTextureType(type);
    desc->setPixelFormat(foramt);
    desc->setWidth(width);
    desc->setHeight(height);
    desc->setDepth(depth);
    desc->setMipmapLevelCount(mipmap);
    desc->setAllowGPUOptimizedContents(true);
    desc->setStorageMode(MTL::StorageModePrivate);
    desc->setHazardTrackingMode(MTL::HazardTrackingModeTracked);
    desc->setUsage(MTL::TextureUsageShaderRead | MTL::TextureUsageShaderWrite);
    return device->newTexture(desc);
}

MetalTexture::MetalTexture(MTL::Texture* ptr, bool isRetain)
    : texture(ptr) {
    if (isRetain) {
        ptr->retain();
    }
}

MetalTexture::MetalTexture(
    MTL::Device* device,
    MTL::PixelFormat foramt,
    MTL::TextureType type,
    uint width, uint height,
    uint depth,
    uint mipmap)
    : texture(CreateTextureImpl(device, foramt, type, width, height, depth, mipmap)) {}

MetalTexture::~MetalTexture() noexcept {
    if (texture != nullptr) {
        texture->release();
        texture = nullptr;
    }
}

}  // namespace radray::rhi::metal
