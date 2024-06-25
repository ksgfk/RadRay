#include "metal_texture.h"

#include "metal_device.h"

namespace radray::rhi::metal {

MetalTexture::MetalTexture(NS::SharedPtr<MTL::Texture> ptr)
    : texture(std::move(ptr)) {}

MetalTexture::MetalTexture(
    MetalDevice* device,
    MTL::PixelFormat foramt,
    uint dimension,
    uint width, uint height,
    uint depth,
    uint mipmap) {
    NS::SharedPtr<MTL::TextureDescriptor> desc = NS::TransferPtr(MTL::TextureDescriptor::alloc()->init());
    desc->setTextureType(([&]() {
        switch (dimension) {
            case 1u: return MTL::TextureType1D;
            case 2u: return MTL::TextureType2D;
            case 3u: return MTL::TextureType3D;
            default: RADRAY_ABORT("invalid metal texture dim {}", dimension); return (MTL::TextureType)-1;
        }
    })());
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
