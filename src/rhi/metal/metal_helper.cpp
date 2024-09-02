#include "metal_helper.h"

namespace radray::rhi::metal {

MTL::PixelFormat EnumConvert(RadrayFormat format) noexcept {
    switch (format) {
        case RADRAY_FORMAT_R8_SINT: return MTL::PixelFormatR8Sint;
        case RADRAY_FORMAT_R8_UINT: return MTL::PixelFormatR8Uint;
        case RADRAY_FORMAT_R8_UNORM: return MTL::PixelFormatR8Unorm;
        case RADRAY_FORMAT_RG8_SINT: return MTL::PixelFormatRG8Sint;
        case RADRAY_FORMAT_RG8_UINT: return MTL::PixelFormatRG8Uint;
        case RADRAY_FORMAT_RG8_UNORM: return MTL::PixelFormatRG8Unorm;
        case RADRAY_FORMAT_RGBA8_SINT: return MTL::PixelFormatRGBA8Sint;
        case RADRAY_FORMAT_RGBA8_UINT: return MTL::PixelFormatRGBA8Uint;
        case RADRAY_FORMAT_RGBA8_UNORM: return MTL::PixelFormatRGBA8Unorm;
        case RADRAY_FORMAT_R16_SINT: return MTL::PixelFormatR16Sint;
        case RADRAY_FORMAT_R16_UINT: return MTL::PixelFormatR16Uint;
        case RADRAY_FORMAT_R16_UNORM: return MTL::PixelFormatR16Unorm;
        case RADRAY_FORMAT_RG16_SINT: return MTL::PixelFormatRG16Sint;
        case RADRAY_FORMAT_RG16_UINT: return MTL::PixelFormatRG16Uint;
        case RADRAY_FORMAT_RG16_UNORM: return MTL::PixelFormatRG16Unorm;
        case RADRAY_FORMAT_RGBA16_SINT: return MTL::PixelFormatRGBA16Sint;
        case RADRAY_FORMAT_RGBA16_UINT: return MTL::PixelFormatRGBA16Uint;
        case RADRAY_FORMAT_RGBA16_UNORM: return MTL::PixelFormatRGBA16Unorm;
        case RADRAY_FORMAT_R32_SINT: return MTL::PixelFormatR32Sint;
        case RADRAY_FORMAT_R32_UINT: return MTL::PixelFormatR32Uint;
        case RADRAY_FORMAT_RG32_SINT: return MTL::PixelFormatRG32Sint;
        case RADRAY_FORMAT_RG32_UINT: return MTL::PixelFormatRG32Uint;
        case RADRAY_FORMAT_RGBA32_SINT: return MTL::PixelFormatRGBA32Sint;
        case RADRAY_FORMAT_RGBA32_UINT: return MTL::PixelFormatRGBA32Uint;
        case RADRAY_FORMAT_R16_FLOAT: return MTL::PixelFormatR16Float;
        case RADRAY_FORMAT_RG16_FLOAT: return MTL::PixelFormatRG16Float;
        case RADRAY_FORMAT_RGBA16_FLOAT: return MTL::PixelFormatRGBA16Float;
        case RADRAY_FORMAT_R32_FLOAT: return MTL::PixelFormatR32Float;
        case RADRAY_FORMAT_RG32_FLOAT: return MTL::PixelFormatRG32Float;
        case RADRAY_FORMAT_RGBA32_FLOAT: return MTL::PixelFormatRGBA32Float;
        case RADRAY_FORMAT_R10G10B10A2_UINT: return MTL::PixelFormatRGB10A2Uint;
        case RADRAY_FORMAT_R10G10B10A2_UNORM: return MTL::PixelFormatRGB10A2Unorm;
        case RADRAY_FORMAT_R11G11B10_FLOAT: return MTL::PixelFormatRG11B10Float;
        case RADRAY_FORMAT_D16_UNORM: return MTL::PixelFormatDepth16Unorm;
        case RADRAY_FORMAT_D32_FLOAT: return MTL::PixelFormatDepth32Float;
        case RADRAY_FORMAT_D24_UNORM_S8_UINT: return MTL::PixelFormatDepth24Unorm_Stencil8;
        case RADRAY_FORMAT_D32_FLOAT_S8_UINT: return MTL::PixelFormatDepth32Float_Stencil8;
        default: return MTL::PixelFormatInvalid;
    }
}

}  // namespace radray::rhi::metal
