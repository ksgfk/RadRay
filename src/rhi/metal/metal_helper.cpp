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
        case RADRAY_FORMAT_BGRA8_UNORM: return MTL::PixelFormatBGRA8Unorm;
        case RADRAY_FORMAT_UNKNOWN: return MTL::PixelFormatInvalid;
    }
}

NS::UInteger EnumConvert(RadrayTextureMSAACount cnt) noexcept {
    switch (cnt) {
        case RADRAY_TEXTURE_MSAA_1: return 1;
        case RADRAY_TEXTURE_MSAA_2: return 2;
        case RADRAY_TEXTURE_MSAA_4: return 4;
        case RADRAY_TEXTURE_MSAA_8: return 8;
        case RADRAY_TEXTURE_MSAA_16: return 16;
    }
}

MTL::TextureType EnumConvert(RadrayTextureDimension dim) noexcept {
    switch (dim) {
        case RADRAY_TEXTURE_DIM_1D: return MTL::TextureType1D;
        case RADRAY_TEXTURE_DIM_2D: return MTL::TextureType2D;
        case RADRAY_TEXTURE_DIM_3D: return MTL::TextureType3D;
        case RADRAY_TEXTURE_DIM_CUBE: return MTL::TextureTypeCube;
        case RADRAY_TEXTURE_DIM_1D_ARRAY: return MTL::TextureType1DArray;
        case RADRAY_TEXTURE_DIM_2D_ARRAY: return MTL::TextureType2DArray;
        case RADRAY_TEXTURE_DIM_CUBE_ARRAY: return MTL::TextureTypeCubeArray;
        case RADRAY_TEXTURE_DIM_UNKNOWN: return (MTL::TextureType)-1;
    }
}

MTL::LoadAction EnumConvert(RadrayLoadAction load) noexcept {
    switch (load) {
        case RADRAY_LOAD_ACTION_DONTCARE: return MTL::LoadActionDontCare;
        case RADRAY_LOAD_ACTION_LOAD: return MTL::LoadActionLoad;
        case RADRAY_LOAD_ACTION_CLEAR: return MTL::LoadActionClear;
    }
}

MTL::StoreAction EnumConvert(RadrayStoreAction store) noexcept {
    switch (store) {
        case RADRAY_STORE_ACTION_STORE: return MTL::StoreActionStore;
        case RADRAY_STORE_ACTION_DISCARD: return MTL::StoreActionDontCare;
    }
}

MTL::VertexFormat EnumConvert(RadrayVertexFormat format) noexcept {
    switch (format) {
        case RADRAY_VERTEX_FORMAT_FLOAT1: return MTL::VertexFormatFloat;
        case RADRAY_VERTEX_FORMAT_FLOAT2: return MTL::VertexFormatFloat2;
        case RADRAY_VERTEX_FORMAT_FLOAT3: return MTL::VertexFormatFloat3;
        case RADRAY_VERTEX_FORMAT_FLOAT4: return MTL::VertexFormatFloat4;
        case RADRAY_VERTEX_FORMAT_INT1: return MTL::VertexFormatInt;
        case RADRAY_VERTEX_FORMAT_INT2: return MTL::VertexFormatInt2;
        case RADRAY_VERTEX_FORMAT_INT3: return MTL::VertexFormatInt3;
        case RADRAY_VERTEX_FORMAT_INT4: return MTL::VertexFormatInt4;
        case RADRAY_VERTEX_FORMAT_UINT1: return MTL::VertexFormatUInt;
        case RADRAY_VERTEX_FORMAT_UINT2: return MTL::VertexFormatUInt2;
        case RADRAY_VERTEX_FORMAT_UINT3: return MTL::VertexFormatUInt3;
        case RADRAY_VERTEX_FORMAT_UINT4: return MTL::VertexFormatUInt4;
    }
}

MTL::VertexStepFunction EnumConvert(RadrayVertexInputRate rate) noexcept {
    switch (rate) {
        case RADRAY_INPUT_RATE_VERTEX: return MTL::VertexStepFunctionPerVertex;
        case RADRAY_INPUT_RATE_INSTANCE: return MTL::VertexStepFunctionPerInstance;
    }
}

}  // namespace radray::rhi::metal
