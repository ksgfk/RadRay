#include "metal_helper.h"

namespace radray::rhi::metal {

MTL::PixelFormat ToMtlFormat(PixelFormat format) noexcept {
    switch (format) {
        case PixelFormat::Unknown: return MTL::PixelFormat::PixelFormatInvalid;
        case PixelFormat::R8_SInt: return MTL::PixelFormat::PixelFormatR8Sint;
        case PixelFormat::R8_UInt: return MTL::PixelFormat::PixelFormatR8Uint;
        case PixelFormat::R8_UNorm: return MTL::PixelFormat::PixelFormatR8Unorm;
        case PixelFormat::RG8_SInt: return MTL::PixelFormat::PixelFormatRG8Sint;
        case PixelFormat::RG8_UInt: return MTL::PixelFormat::PixelFormatRG8Uint;
        case PixelFormat::RG8_UNorm: return MTL::PixelFormat::PixelFormatRG8Unorm;
        case PixelFormat::RGBA8_SInt: return MTL::PixelFormat::PixelFormatRGBA8Sint;
        case PixelFormat::RGBA8_UInt: return MTL::PixelFormat::PixelFormatRGBA8Uint;
        case PixelFormat::RGBA8_UNorm: return MTL::PixelFormat::PixelFormatRGBA8Unorm;
        case PixelFormat::R16_SInt: return MTL::PixelFormat::PixelFormatR16Sint;
        case PixelFormat::R16_UInt: return MTL::PixelFormat::PixelFormatR16Uint;
        case PixelFormat::R16_UNorm: return MTL::PixelFormat::PixelFormatR16Unorm;
        case PixelFormat::RG16_SInt: return MTL::PixelFormat::PixelFormatRG16Sint;
        case PixelFormat::RG16_UInt: return MTL::PixelFormat::PixelFormatRG16Uint;
        case PixelFormat::RG16_UNorm: return MTL::PixelFormat::PixelFormatRG16Unorm;
        case PixelFormat::RGBA16_SInt: return MTL::PixelFormat::PixelFormatRGBA16Sint;
        case PixelFormat::RGBA16_UInt: return MTL::PixelFormat::PixelFormatRGBA16Uint;
        case PixelFormat::RGBA16_UNorm: return MTL::PixelFormat::PixelFormatRGBA16Unorm;
        case PixelFormat::R32_SInt: return MTL::PixelFormat::PixelFormatR32Sint;
        case PixelFormat::R32_UInt: return MTL::PixelFormat::PixelFormatR32Uint;
        case PixelFormat::RG32_SInt: return MTL::PixelFormat::PixelFormatRG32Sint;
        case PixelFormat::RG32_UInt: return MTL::PixelFormat::PixelFormatRG32Uint;
        case PixelFormat::RGBA32_SInt: return MTL::PixelFormat::PixelFormatRGBA32Sint;
        case PixelFormat::RGBA32_UInt: return MTL::PixelFormat::PixelFormatRGBA32Uint;
        case PixelFormat::R16_Float: return MTL::PixelFormat::PixelFormatR16Float;
        case PixelFormat::RG16_Float: return MTL::PixelFormat::PixelFormatRG16Float;
        case PixelFormat::RGBA16_Float: return MTL::PixelFormat::PixelFormatRGBA16Float;
        case PixelFormat::R32_Float: return MTL::PixelFormat::PixelFormatR32Float;
        case PixelFormat::RG32_Float: return MTL::PixelFormat::PixelFormatRG32Float;
        case PixelFormat::RGBA32_Float: return MTL::PixelFormat::PixelFormatRGBA32Float;
        case PixelFormat::R10G10B10A2_UInt: return MTL::PixelFormat::PixelFormatRGB10A2Uint;
        case PixelFormat::R10G10B10A2_UNorm: return MTL::PixelFormat::PixelFormatRGB10A2Unorm;
        case PixelFormat::R11G11B10_Float: return MTL::PixelFormat::PixelFormatRG11B10Float;
        case PixelFormat::D16_UNorm: return MTL::PixelFormat::PixelFormatDepth16Unorm;
        case PixelFormat::D32_Float: return MTL::PixelFormat::PixelFormatDepth32Float;
        case PixelFormat::D24S8: return MTL::PixelFormat::PixelFormatDepth24Unorm_Stencil8;
        case PixelFormat::D32S8: return MTL::PixelFormat::PixelFormatDepth32Float_Stencil8;
    }
}

PixelFormat ToRhiFormat(MTL::PixelFormat format) noexcept {
    switch (format) {
        case MTL::PixelFormat::PixelFormatInvalid: return PixelFormat::Unknown;
        case MTL::PixelFormat::PixelFormatR8Sint: return PixelFormat::R8_SInt;
        case MTL::PixelFormat::PixelFormatR8Uint: return PixelFormat::R8_UInt;
        case MTL::PixelFormat::PixelFormatR8Unorm: return PixelFormat::R8_UNorm;
        case MTL::PixelFormat::PixelFormatRG8Sint: return PixelFormat::RG8_SInt;
        case MTL::PixelFormat::PixelFormatRG8Uint: return PixelFormat::RG8_UInt;
        case MTL::PixelFormat::PixelFormatRG8Unorm: return PixelFormat::RG8_UNorm;
        case MTL::PixelFormat::PixelFormatRGBA8Sint: return PixelFormat::RGBA8_SInt;
        case MTL::PixelFormat::PixelFormatRGBA8Uint: return PixelFormat::RGBA8_UInt;
        case MTL::PixelFormat::PixelFormatRGBA8Unorm: return PixelFormat::RGBA8_UNorm;
        case MTL::PixelFormat::PixelFormatR16Sint: return PixelFormat::R16_SInt;
        case MTL::PixelFormat::PixelFormatR16Uint: return PixelFormat::R16_UInt;
        case MTL::PixelFormat::PixelFormatR16Unorm: return PixelFormat::R16_UNorm;
        case MTL::PixelFormat::PixelFormatRG16Sint: return PixelFormat::RG16_SInt;
        case MTL::PixelFormat::PixelFormatRG16Uint: return PixelFormat::RG16_UInt;
        case MTL::PixelFormat::PixelFormatRG16Unorm: return PixelFormat::RG16_UNorm;
        case MTL::PixelFormat::PixelFormatRGBA16Sint: return PixelFormat::RGBA16_SInt;
        case MTL::PixelFormat::PixelFormatRGBA16Uint: return PixelFormat::RGBA16_UInt;
        case MTL::PixelFormat::PixelFormatRGBA16Unorm: return PixelFormat::RGBA16_UNorm;
        case MTL::PixelFormat::PixelFormatR32Sint: return PixelFormat::R32_SInt;
        case MTL::PixelFormat::PixelFormatR32Uint: return PixelFormat::R32_UInt;
        case MTL::PixelFormat::PixelFormatRG32Sint: return PixelFormat::RG32_SInt;
        case MTL::PixelFormat::PixelFormatRG32Uint: return PixelFormat::RG32_UInt;
        case MTL::PixelFormat::PixelFormatRGBA32Sint: return PixelFormat::RGBA32_SInt;
        case MTL::PixelFormat::PixelFormatRGBA32Uint: return PixelFormat::RGBA32_UInt;
        case MTL::PixelFormat::PixelFormatR16Float: return PixelFormat::R16_Float;
        case MTL::PixelFormat::PixelFormatRG16Float: return PixelFormat::RG16_Float;
        case MTL::PixelFormat::PixelFormatRGBA16Float: return PixelFormat::RGBA16_Float;
        case MTL::PixelFormat::PixelFormatR32Float: return PixelFormat::R32_Float;
        case MTL::PixelFormat::PixelFormatRG32Float: return PixelFormat::RG32_Float;
        case MTL::PixelFormat::PixelFormatRGBA32Float: return PixelFormat::RGBA32_Float;
        case MTL::PixelFormat::PixelFormatRGB10A2Uint: return PixelFormat::R10G10B10A2_UInt;
        case MTL::PixelFormat::PixelFormatRGB10A2Unorm: return PixelFormat::R10G10B10A2_UNorm;
        case MTL::PixelFormat::PixelFormatRG11B10Float: return PixelFormat::R11G11B10_Float;
        case MTL::PixelFormat::PixelFormatDepth16Unorm: return PixelFormat::D16_UNorm;
        case MTL::PixelFormat::PixelFormatDepth32Float: return PixelFormat::D32_Float;
        case MTL::PixelFormat::PixelFormatDepth24Unorm_Stencil8: return PixelFormat::D24S8;
        case MTL::PixelFormat::PixelFormatDepth32Float_Stencil8: return PixelFormat::D32S8;
        default: return PixelFormat::Unknown;
    }
}

MTL::TextureType ToMtlTextureType(TextureDimension dim) noexcept {
    switch (dim) {
        case TextureDimension::Tex_1D: return MTL::TextureType1D;
        case TextureDimension::Tex_2D: return MTL::TextureType2D;
        case TextureDimension::Tex_3D: return MTL::TextureType3D;
        case TextureDimension::Cubemap: return MTL::TextureTypeCube;
        case TextureDimension::Tex_2D_Array: return MTL::TextureType2DArray;
    }
}

TextureDimension ToRhiDimension(MTL::TextureType type) noexcept {
    switch (type) {
        case MTL::TextureType1D: return TextureDimension::Tex_1D;
        case MTL::TextureType2D: return TextureDimension::Tex_2D;
        case MTL::TextureType3D: return TextureDimension::Tex_3D;
        case MTL::TextureTypeCube: return TextureDimension::Cubemap;
        case MTL::TextureType2DArray: return TextureDimension::Tex_2D_Array;
        default: return (TextureDimension)-1;
    }
}

MTL::VertexFormat ToMtlVertexFormat(InputElementFormat format) noexcept {
    switch (format) {
        case InputElementFormat::Float: return MTL::VertexFormat::VertexFormatFloat;
        case InputElementFormat::Float2: return MTL::VertexFormat::VertexFormatFloat2;
        case InputElementFormat::Float3: return MTL::VertexFormat::VertexFormatFloat3;
        case InputElementFormat::Float4: return MTL::VertexFormat::VertexFormatFloat4;
        case InputElementFormat::Int: return MTL::VertexFormat::VertexFormatInt;
        case InputElementFormat::Int2: return MTL::VertexFormat::VertexFormatInt2;
        case InputElementFormat::Int3: return MTL::VertexFormat::VertexFormatInt3;
        case InputElementFormat::Int4: return MTL::VertexFormat::VertexFormatInt4;
        case InputElementFormat::UInt: return MTL::VertexFormat::VertexFormatUInt;
        case InputElementFormat::UInt2: return MTL::VertexFormat::VertexFormatUInt2;
        case InputElementFormat::UInt3: return MTL::VertexFormat::VertexFormatUInt3;
        case InputElementFormat::UInt4: return MTL::VertexFormat::VertexFormatUInt4;
        default: return MTL::VertexFormat::VertexFormatInvalid;
    }
}

MTL::BlendFactor ToMtlBlendFactor(BlendType type) noexcept {
    switch (type) {
        case BlendType::Zero: return MTL::BlendFactor::BlendFactorZero;
        case BlendType::One: return MTL::BlendFactor::BlendFactorOne;
        case BlendType::Src_Color: return MTL::BlendFactor::BlendFactorSourceColor;
        case BlendType::Inv_Src_Color: return MTL::BlendFactor::BlendFactorOneMinusSourceColor;
        case BlendType::Src_Alpha: return MTL::BlendFactor::BlendFactorSourceAlpha;
        case BlendType::Inv_Src_Alpha: return MTL::BlendFactor::BlendFactorOneMinusSourceAlpha;
        case BlendType::Dest_Alpha: return MTL::BlendFactor::BlendFactorDestinationAlpha;
        case BlendType::Inv_Dest_Alpha: return MTL::BlendFactor::BlendFactorOneMinusDestinationAlpha;
        case BlendType::Dest_Color: return MTL::BlendFactor::BlendFactorDestinationColor;
        case BlendType::Inv_Dest_Color: return MTL::BlendFactor::BlendFactorOneMinusDestinationColor;
        case BlendType::Src_Alpha_Sat: return MTL::BlendFactor::BlendFactorSourceAlphaSaturated;
        case BlendType::Blend_Factor: return MTL::BlendFactor::BlendFactorBlendColor;
        case BlendType::Inv_Blend_Factor: return MTL::BlendFactor::BlendFactorOneMinusBlendColor;
        case BlendType::Src1_Color: return MTL::BlendFactor::BlendFactorSource1Color;
        case BlendType::Inv_Src1_Color: return MTL::BlendFactor::BlendFactorOneMinusSource1Color;
        case BlendType::Src1_Alpha: return MTL::BlendFactor::BlendFactorSource1Alpha;
        case BlendType::Inv_Src1_Alpha: return MTL::BlendFactor::BlendFactorOneMinusSource1Alpha;
        case BlendType::Alpha_Factor: return MTL::BlendFactor::BlendFactorBlendAlpha;
        case BlendType::Inv_Alpha_Factor: return MTL::BlendFactor::BlendFactorOneMinusBlendAlpha;
        default: return (MTL::BlendFactor)-1;
    }
}

MTL::BlendOperation ToMtlBlendOp(BlendOpMode mode) noexcept {
    switch (mode) {
        case BlendOpMode::Add: return MTL::BlendOperation::BlendOperationAdd;
        case BlendOpMode::Subtract: return MTL::BlendOperation::BlendOperationSubtract;
        case BlendOpMode::Rev_Subtract: return MTL::BlendOperation::BlendOperationReverseSubtract;
        case BlendOpMode::Min: return MTL::BlendOperation::BlendOperationMin;
        case BlendOpMode::Max: return MTL::BlendOperation::BlendOperationMax;
        default: return (MTL::BlendOperation)-1;
    }
}

MTL::ColorWriteMask ToMtlColorWriteMask(ColorWriteEnable value) noexcept {
    switch (value) {
        case ColorWriteEnable::Red: return MTL::ColorWriteMaskRed;
        case ColorWriteEnable::Green: return MTL::ColorWriteMaskGreen;
        case ColorWriteEnable::Blue: return MTL::ColorWriteMaskBlue;
        case ColorWriteEnable::Alpha: return MTL::ColorWriteMaskAlpha;
        case ColorWriteEnable::All: return MTL::ColorWriteMaskAll;
        default: return (MTL::ColorWriteMask)-1;
    }
}

MTL::PrimitiveTopologyClass ToMtlPrimitiveTopology(PrimitiveTopology topo) noexcept {
    switch (topo) {
        case PrimitiveTopology::Point_List: return MTL::PrimitiveTopologyClass::PrimitiveTopologyClassPoint;
        case PrimitiveTopology::Line_List: return MTL::PrimitiveTopologyClass::PrimitiveTopologyClassLine;
        case PrimitiveTopology::Line_Strip: return MTL::PrimitiveTopologyClass::PrimitiveTopologyClassLine;
        case PrimitiveTopology::Triangle_List: return MTL::PrimitiveTopologyClass::PrimitiveTopologyClassTriangle;
        case PrimitiveTopology::Triangle_Strip: return MTL::PrimitiveTopologyClass::PrimitiveTopologyClassTriangle;
        default: return MTL::PrimitiveTopologyClass::PrimitiveTopologyClassUnspecified;
    }
}

}  // namespace radray::rhi::metal
