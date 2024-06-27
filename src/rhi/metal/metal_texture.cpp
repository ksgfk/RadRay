#include "metal_texture.h"

#include "metal_device.h"

namespace radray::rhi::metal {

MetalTexture::MetalTexture(NS::SharedPtr<MTL::Texture> ptr)
    : texture(std::move(ptr)) {}

MetalTexture::MetalTexture(
    MetalDevice* device,
    MTL::PixelFormat foramt,
    MTL::TextureType type,
    uint width, uint height,
    uint depth,
    uint mipmap) {
    NS::SharedPtr<MTL::TextureDescriptor> desc = NS::TransferPtr(MTL::TextureDescriptor::alloc()->init());
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
    auto tex = device->device->newTexture(desc.get());
    texture = NS::TransferPtr(tex);
}

}  // namespace radray::rhi::metal
