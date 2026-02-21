#pragma once

#ifdef RADRAY_ENABLE_METAL

#ifndef __OBJC__
#error "This header can only be included in Objective-C++ files."
#endif
#import <MetalKit/MetalKit.h>
#import <QuartzCore/CAMetalLayer.h>

#include <radray/render/common.h>

namespace radray::render::metal {

MTLPixelFormat MapPixelFormat(TextureFormat v) noexcept;
MTLLoadAction MapLoadAction(LoadAction v) noexcept;
MTLStoreAction MapStoreAction(StoreAction v) noexcept;
MTLTextureType MapTextureType(TextureDimension dim, uint32_t sampleCount) noexcept;
MTLTextureType MapTextureViewType(TextureDimension dim) noexcept;
MTLResourceOptions MapResourceOptions(MemoryType mem) noexcept;
MTLStorageMode MapStorageMode(MemoryType mem) noexcept;
MTLCompareFunction MapCompareFunction(CompareFunction v) noexcept;
MTLSamplerAddressMode MapAddressMode(AddressMode v) noexcept;
MTLSamplerMinMagFilter MapMinMagFilter(FilterMode v) noexcept;
MTLSamplerMipFilter MapMipFilter(FilterMode v) noexcept;
MTLBlendFactor MapBlendFactor(BlendFactor v) noexcept;
MTLBlendOperation MapBlendOperation(BlendOperation v) noexcept;
MTLStencilOperation MapStencilOp(StencilOperation v) noexcept;
MTLVertexFormat MapVertexFormat(VertexFormat v) noexcept;
MTLVertexStepFunction MapVertexStepFunction(VertexStepMode v) noexcept;
MTLPrimitiveType MapPrimitiveType(PrimitiveTopology v) noexcept;
MTLIndexType MapIndexType(IndexFormat v) noexcept;
MTLCullMode MapCullMode(CullMode v) noexcept;
MTLWinding MapWinding(FrontFace v) noexcept;
MTLColorWriteMask MapColorWriteMask(ColorWrites mask) noexcept;

struct ArgumentDescriptorInfo {
    MTLDataType dataType;
    MTLBindingAccess access;
};
ArgumentDescriptorInfo MapResourceBindTypeToArgument(ResourceBindType type) noexcept;

}  // namespace radray::render::metal

std::string_view format_as(MTLLanguageVersion v) noexcept;
std::string_view format_as(MTLArgumentBuffersTier v) noexcept;

#endif  // RADRAY_ENABLE_METAL
