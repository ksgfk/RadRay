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

MTL::BlendFactor EnumConvert(RadrayBlendType blend) noexcept {
    switch (blend) {
        case RADRAY_BLEND_TYPE_ZERO: return MTL::BlendFactorZero;
        case RADRAY_BLEND_TYPE_ONE: return MTL::BlendFactorOne;
        case RADRAY_BLEND_TYPE_SRC_COLOR: return MTL::BlendFactorSourceColor;
        case RADRAY_BLEND_TYPE_INV_SRC_COLOR: return MTL::BlendFactorOneMinusSourceColor;
        case RADRAY_BLEND_TYPE_SRC_ALPHA: return MTL::BlendFactorSourceAlpha;
        case RADRAY_BLEND_TYPE_INV_SRC_ALPHA: return MTL::BlendFactorOneMinusSourceAlpha;
        case RADRAY_BLEND_TYPE_DST_COLOR: return MTL::BlendFactorDestinationColor;
        case RADRAY_BLEND_TYPE_INV_DST_COLOR: return MTL::BlendFactorOneMinusDestinationColor;
        case RADRAY_BLEND_TYPE_DST_ALPHA: return MTL::BlendFactorDestinationAlpha;
        case RADRAY_BLEND_TYPE_INV_DST_ALPHA: return MTL::BlendFactorOneMinusDestinationAlpha;
        case RADRAY_BLEND_TYPE_SRC_ALPHA_SAT: return MTL::BlendFactorSourceAlphaSaturated;
        case RADRAY_BLEND_TYPE_CONSTANT: return MTL::BlendFactorBlendColor;
        case RADRAY_BLEND_TYPE_INV_CONSTANT: return MTL::BlendFactorOneMinusBlendColor;
        case RADRAY_BLEND_TYPE_SRC1_COLOR: return MTL::BlendFactorSource1Color;
        case RADRAY_BLEND_TYPE_INV_SRC1_COLOR: return MTL::BlendFactorOneMinusSource1Color;
        case RADRAY_BLEND_TYPE_SRC1_ALPHA: return MTL::BlendFactorSource1Alpha;
        case RADRAY_BLEND_TYPE_INV_SRC1_ALPHA: return MTL::BlendFactorOneMinusSource1Alpha;
    }
}

MTL::BlendOperation EnumConvert(RadrayBlendOp op) noexcept {
    switch (op) {
        case RADRAY_BLEND_OP_ADD: return MTL::BlendOperationAdd;
        case RADRAY_BLEND_OP_SUBTRACT: return MTL::BlendOperationSubtract;
        case RADRAY_BLEND_OP_REVERSE_SUBTRACT: return MTL::BlendOperationReverseSubtract;
        case RADRAY_BLEND_OP_MIN: return MTL::BlendOperationMin;
        case RADRAY_BLEND_OP_MAX: return MTL::BlendOperationMax;
    }
}

std::pair<MTL::PrimitiveTopologyClass, MTL::PrimitiveType> EnumConvert(RadrayPrimitiveTopology topo) noexcept {
    switch (topo) {
        case RADRAY_PRIMITIVE_TOPOLOGY_POINT_LIST: return std::make_pair(MTL::PrimitiveTopologyClassPoint, MTL::PrimitiveTypePoint);
        case RADRAY_PRIMITIVE_TOPOLOGY_LINE_LIST: return std::make_pair(MTL::PrimitiveTopologyClassLine, MTL::PrimitiveTypeLine);
        case RADRAY_PRIMITIVE_TOPOLOGY_LINE_STRIP: return std::make_pair(MTL::PrimitiveTopologyClassLine, MTL::PrimitiveTypeLineStrip);
        case RADRAY_PRIMITIVE_TOPOLOGY_TRI_LIST: return std::make_pair(MTL::PrimitiveTopologyClassTriangle, MTL::PrimitiveTypeTriangle);
        case RADRAY_PRIMITIVE_TOPOLOGY_TRI_STRIP: return std::make_pair(MTL::PrimitiveTopologyClassTriangle, MTL::PrimitiveTypeTriangleStrip);
    }
}

MTL::TriangleFillMode EnumConvert(RadrayPolygonMode poly) noexcept {
    switch (poly) {
        case RADRAY_POLYGON_MODE_FILL: return MTL::TriangleFillModeFill;
        case RADRAY_POLYGON_MODE_LINE: return MTL::TriangleFillModeLines;
        case RADRAY_POLYGON_MODE_POINT: return (MTL::TriangleFillMode)-1;
    }
}

MTL::ColorWriteMask EnumConvert(RadrayColorWrites bits) noexcept {
    MTL::ColorWriteMask result = MTL::ColorWriteMaskNone;
    if (bits & RADRAY_COLOR_WRITE_RED) {
        result |= MTL::ColorWriteMaskRed;
    }
    if (bits & RADRAY_COLOR_WRITE_GREEN) {
        result |= MTL::ColorWriteMaskGreen;
    }
    if (bits & RADRAY_COLOR_WRITE_BLUE) {
        result |= MTL::ColorWriteMaskBlue;
    }
    if (bits & RADRAY_COLOR_WRITE_ALPHA) {
        result |= MTL::ColorWriteMaskAlpha;
    }
    return result;
}

std::tuple<MTL::BlendOperation, MTL::BlendFactor, MTL::BlendFactor> EnumConvert(const RadrayBlendComponentState& s) noexcept {
    return std::make_tuple(
        EnumConvert(s.Operation),
        EnumConvert(s.SrcFactor),
        EnumConvert(s.DstFactor));
}

}  // namespace radray::rhi::metal
