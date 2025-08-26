#include "metal_helper.h"

SEL NSString_initWithBytes_length_encoding_() {
    static SEL sel = sel_registerName("initWithBytes:length:encoding:");
    return sel;
}

class DummyNS : public NS::Object {
public:
    template <typename R, typename... Args>
    static R hackSendMessage(const void* pObj, SEL selector, Args&&... args) {
        return sendMessage<R>(pObj, selector, std::forward<Args>(args)...);
    }
};

namespace radray::render::metal {

ScopedAutoreleasePool::ScopedAutoreleasePool() noexcept
    : _pool{NS::AutoreleasePool::alloc()->init()} {}

ScopedAutoreleasePool::~ScopedAutoreleasePool() noexcept {
    _pool->release();
}

NS::String* NSStringInit(NS::String* that, const void* bytes, NS::UInteger len, NS::StringEncoding encoding) noexcept {
    return DummyNS::hackSendMessage<NS::String*>(that, NSString_initWithBytes_length_encoding_(), bytes, len, encoding);
}

NS::String* StringCppToNS(std::string_view str) noexcept {
    return NSStringInit(
        NS::String::alloc(),
        reinterpret_cast<const void*>(str.data()),
        str.size(),
        NS::StringEncoding::UTF8StringEncoding);
}

std::optional<MTL::TriangleFillMode> MapType(PolygonMode v) noexcept {
    switch (v) {
        case PolygonMode::Fill: return MTL::TriangleFillModeFill;
        case PolygonMode::Line: return MTL::TriangleFillModeLines;
        default: return std::nullopt;
    }
}

std::pair<MTL::PrimitiveTopologyClass, MTL::PrimitiveType> MapType(PrimitiveTopology v) noexcept {
    switch (v) {
        case PrimitiveTopology::PointList: return std::make_pair(MTL::PrimitiveTopologyClassPoint, MTL::PrimitiveTypePoint);
        case PrimitiveTopology::LineList: return std::make_pair(MTL::PrimitiveTopologyClassLine, MTL::PrimitiveTypeLine);
        case PrimitiveTopology::LineStrip: return std::make_pair(MTL::PrimitiveTopologyClassLine, MTL::PrimitiveTypeLineStrip);
        case PrimitiveTopology::TriangleList: return std::make_pair(MTL::PrimitiveTopologyClassTriangle, MTL::PrimitiveTypeTriangle);
        case PrimitiveTopology::TriangleStrip: return std::make_pair(MTL::PrimitiveTopologyClassTriangle, MTL::PrimitiveTypeTriangleStrip);
    }
}

MTL::PixelFormat MapType(TextureFormat v) noexcept {
    switch (v) {
        case TextureFormat::UNKNOWN: return MTL::PixelFormatInvalid;
        case TextureFormat::R8_SINT: return MTL::PixelFormatR8Sint;
        case TextureFormat::R8_UINT: return MTL::PixelFormatR8Uint;
        case TextureFormat::R8_SNORM: return MTL::PixelFormatR8Snorm;
        case TextureFormat::R8_UNORM: return MTL::PixelFormatR8Unorm;
        case TextureFormat::R16_SINT: return MTL::PixelFormatR16Sint;
        case TextureFormat::R16_UINT: return MTL::PixelFormatR16Uint;
        case TextureFormat::R16_SNORM: return MTL::PixelFormatR16Snorm;
        case TextureFormat::R16_UNORM: return MTL::PixelFormatR16Unorm;
        case TextureFormat::R16_FLOAT: return MTL::PixelFormatR16Float;
        case TextureFormat::RG8_SINT: return MTL::PixelFormatRG8Sint;
        case TextureFormat::RG8_UINT: return MTL::PixelFormatRG8Uint;
        case TextureFormat::RG8_SNORM: return MTL::PixelFormatRG8Snorm;
        case TextureFormat::RG8_UNORM: return MTL::PixelFormatRG8Unorm;
        case TextureFormat::R32_SINT: return MTL::PixelFormatR32Sint;
        case TextureFormat::R32_UINT: return MTL::PixelFormatR32Uint;
        case TextureFormat::R32_FLOAT: return MTL::PixelFormatR32Float;
        case TextureFormat::RG16_SINT: return MTL::PixelFormatRG16Sint;
        case TextureFormat::RG16_UINT: return MTL::PixelFormatRG16Uint;
        case TextureFormat::RG16_SNORM: return MTL::PixelFormatRG16Snorm;
        case TextureFormat::RG16_UNORM: return MTL::PixelFormatRG16Unorm;
        case TextureFormat::RG16_FLOAT: return MTL::PixelFormatRG16Float;
        case TextureFormat::RGBA8_SINT: return MTL::PixelFormatRGBA8Sint;
        case TextureFormat::RGBA8_UINT: return MTL::PixelFormatRGBA8Uint;
        case TextureFormat::RGBA8_SNORM: return MTL::PixelFormatRGBA8Snorm;
        case TextureFormat::RGBA8_UNORM: return MTL::PixelFormatRGBA8Unorm;
        case TextureFormat::RGBA8_UNORM_SRGB: return MTL::PixelFormatRGBA8Unorm_sRGB;
        case TextureFormat::BGRA8_UNORM: return MTL::PixelFormatBGRA8Unorm;
        case TextureFormat::BGRA8_UNORM_SRGB: return MTL::PixelFormatBGRA8Unorm_sRGB;
        case TextureFormat::RGB10A2_UINT: return MTL::PixelFormatRGB10A2Uint;
        case TextureFormat::RGB10A2_UNORM: return MTL::PixelFormatRGB10A2Unorm;
        case TextureFormat::RG11B10_FLOAT: return MTL::PixelFormatRG11B10Float;
        case TextureFormat::RG32_SINT: return MTL::PixelFormatRG32Sint;
        case TextureFormat::RG32_UINT: return MTL::PixelFormatRG32Uint;
        case TextureFormat::RG32_FLOAT: return MTL::PixelFormatRG32Float;
        case TextureFormat::RGBA16_SINT: return MTL::PixelFormatRGBA16Sint;
        case TextureFormat::RGBA16_UINT: return MTL::PixelFormatRGBA16Uint;
        case TextureFormat::RGBA16_SNORM: return MTL::PixelFormatRGBA16Snorm;
        case TextureFormat::RGBA16_UNORM: return MTL::PixelFormatRGBA16Unorm;
        case TextureFormat::RGBA16_FLOAT: return MTL::PixelFormatRGBA16Float;
        case TextureFormat::RGBA32_SINT: return MTL::PixelFormatRGBA32Sint;
        case TextureFormat::RGBA32_UINT: return MTL::PixelFormatRGBA32Uint;
        case TextureFormat::RGBA32_FLOAT: return MTL::PixelFormatRGBA32Float;
        case TextureFormat::S8: return MTL::PixelFormatStencil8;
        case TextureFormat::D16_UNORM: return MTL::PixelFormatDepth16Unorm;
        case TextureFormat::D32_FLOAT: return MTL::PixelFormatDepth32Float;
        case TextureFormat::D24_UNORM_S8_UINT: return MTL::PixelFormatDepth24Unorm_Stencil8;
        case TextureFormat::D32_FLOAT_S8_UINT: return MTL::PixelFormatDepth32Float_Stencil8;
    }
}

MTL::ColorWriteMask MapType(ColorWrites v) noexcept {
    MTL::ColorWriteMask mask = MTL::ColorWriteMaskNone;
    if (HasFlag(v, ColorWrite::Red)) mask |= MTL::ColorWriteMaskRed;
    if (HasFlag(v, ColorWrite::Green)) mask |= MTL::ColorWriteMaskGreen;
    if (HasFlag(v, ColorWrite::Blue)) mask |= MTL::ColorWriteMaskBlue;
    if (HasFlag(v, ColorWrite::Alpha)) mask |= MTL::ColorWriteMaskAlpha;
    return mask;
}

MTL::BlendOperation MapType(BlendOperation v) noexcept {
    switch (v) {
        case BlendOperation::Add: return MTL::BlendOperationAdd;
        case BlendOperation::Subtract: return MTL::BlendOperationSubtract;
        case BlendOperation::ReverseSubtract: return MTL::BlendOperationReverseSubtract;
        case BlendOperation::Min: return MTL::BlendOperationMin;
        case BlendOperation::Max: return MTL::BlendOperationMax;
    }
}

MTL::BlendFactor MapType(BlendFactor v) noexcept {
    switch (v) {
        case BlendFactor::Zero: return MTL::BlendFactorZero;
        case BlendFactor::One: return MTL::BlendFactorOne;
        case BlendFactor::Src: return MTL::BlendFactorSourceColor;
        case BlendFactor::OneMinusSrc: return MTL::BlendFactorOneMinusSourceColor;
        case BlendFactor::SrcAlpha: return MTL::BlendFactorSourceAlpha;
        case BlendFactor::OneMinusSrcAlpha: return MTL::BlendFactorOneMinusSourceAlpha;
        case BlendFactor::Dst: return MTL::BlendFactorDestinationColor;
        case BlendFactor::OneMinusDst: return MTL::BlendFactorOneMinusDestinationColor;
        case BlendFactor::DstAlpha: return MTL::BlendFactorDestinationAlpha;
        case BlendFactor::OneMinusDstAlpha: return MTL::BlendFactorOneMinusDestinationAlpha;
        case BlendFactor::SrcAlphaSaturated: return MTL::BlendFactorSourceAlphaSaturated;
        case BlendFactor::Constant: return MTL::BlendFactorBlendColor;
        case BlendFactor::OneMinusConstant: return MTL::BlendFactorOneMinusBlendColor;
        case BlendFactor::Src1: return MTL::BlendFactorSource1Color;
        case BlendFactor::OneMinusSrc1: return MTL::BlendFactorOneMinusSource1Color;
        case BlendFactor::Src1Alpha: return MTL::BlendFactorSource1Alpha;
        case BlendFactor::OneMinusSrc1Alpha: return MTL::BlendFactorOneMinusSource1Alpha;
    }
}

std::tuple<MTL::BlendOperation, MTL::BlendFactor, MTL::BlendFactor> MapType(BlendComponent v) noexcept {
    return std::make_tuple(MapType(v.Op), MapType(v.Src), MapType(v.Dst));
}

MTL::CompareFunction MapType(CompareFunction v) noexcept {
    switch (v) {
        case CompareFunction::Never: return MTL::CompareFunctionNever;
        case CompareFunction::Less: return MTL::CompareFunctionLess;
        case CompareFunction::Equal: return MTL::CompareFunctionEqual;
        case CompareFunction::LessEqual: return MTL::CompareFunctionLessEqual;
        case CompareFunction::Greater: return MTL::CompareFunctionGreater;
        case CompareFunction::NotEqual: return MTL::CompareFunctionNotEqual;
        case CompareFunction::GreaterEqual: return MTL::CompareFunctionGreaterEqual;
        case CompareFunction::Always: return MTL::CompareFunctionAlways;
    }
}

MTL::StencilOperation MapType(StencilOperation v) noexcept {
    switch (v) {
        case StencilOperation::Keep: return MTL::StencilOperationKeep;
        case StencilOperation::Zero: return MTL::StencilOperationZero;
        case StencilOperation::Replace: return MTL::StencilOperationReplace;
        case StencilOperation::Invert: return MTL::StencilOperationInvert;
        case StencilOperation::IncrementClamp: return MTL::StencilOperationIncrementClamp;
        case StencilOperation::DecrementClamp: return MTL::StencilOperationDecrementClamp;
        case StencilOperation::IncrementWrap: return MTL::StencilOperationIncrementWrap;
        case StencilOperation::DecrementWrap: return MTL::StencilOperationDecrementWrap;
    }
}

MTL::VertexStepFunction MapType(VertexStepMode v) noexcept {
    switch (v) {
        case VertexStepMode::Vertex: return MTL::VertexStepFunctionPerVertex;
        case VertexStepMode::Instance: return MTL::VertexStepFunctionPerInstance;
    }
}

MTL::VertexFormat MapType(VertexFormat v) noexcept {
    switch (v) {
        case VertexFormat::UNKNOWN: return MTL::VertexFormatInvalid;
        case VertexFormat::UINT8X2: return MTL::VertexFormatUChar2;
        case VertexFormat::UINT8X4: return MTL::VertexFormatUChar4;
        case VertexFormat::SINT8X2: return MTL::VertexFormatChar2;
        case VertexFormat::SINT8X4: return MTL::VertexFormatChar4;
        case VertexFormat::UNORM8X2: return MTL::VertexFormatUChar2Normalized;
        case VertexFormat::UNORM8X4: return MTL::VertexFormatUChar4Normalized;
        case VertexFormat::SNORM8X2: return MTL::VertexFormatChar2Normalized;
        case VertexFormat::SNORM8X4: return MTL::VertexFormatChar4Normalized;
        case VertexFormat::UINT16X2: return MTL::VertexFormatUShort2;
        case VertexFormat::UINT16X4: return MTL::VertexFormatUShort4;
        case VertexFormat::SINT16X2: return MTL::VertexFormatShort2;
        case VertexFormat::SINT16X4: return MTL::VertexFormatShort4;
        case VertexFormat::UNORM16X2: return MTL::VertexFormatUShort2Normalized;
        case VertexFormat::UNORM16X4: return MTL::VertexFormatUShort4Normalized;
        case VertexFormat::SNORM16X2: return MTL::VertexFormatShort2Normalized;
        case VertexFormat::SNORM16X4: return MTL::VertexFormatShort4Normalized;
        case VertexFormat::FLOAT16X2: return MTL::VertexFormatHalf2;
        case VertexFormat::FLOAT16X4: return MTL::VertexFormatHalf4;
        case VertexFormat::FLOAT32: return MTL::VertexFormatFloat;
        case VertexFormat::FLOAT32X2: return MTL::VertexFormatFloat2;
        case VertexFormat::FLOAT32X3: return MTL::VertexFormatFloat3;
        case VertexFormat::FLOAT32X4: return MTL::VertexFormatFloat4;
        case VertexFormat::UINT32: return MTL::VertexFormatUInt;
        case VertexFormat::UINT32X2: return MTL::VertexFormatUInt2;
        case VertexFormat::UINT32X3: return MTL::VertexFormatUInt3;
        case VertexFormat::UINT32X4: return MTL::VertexFormatUInt4;
        case VertexFormat::SINT32: return MTL::VertexFormatInt;
        case VertexFormat::SINT32X2: return MTL::VertexFormatInt2;
        case VertexFormat::SINT32X3: return MTL::VertexFormatInt3;
        case VertexFormat::SINT32X4: return MTL::VertexFormatInt4;
    }
}

MTL::Winding MapType(FrontFace v) noexcept {
    switch (v) {
        case FrontFace::CCW: return MTL::WindingCounterClockwise;
        case FrontFace::CW: return MTL::WindingClockwise;
    }
}

MTL::CullMode MapType(CullMode v) noexcept {
    switch (v) {
        case CullMode::None: return MTL::CullModeNone;
        case CullMode::Front: return MTL::CullModeFront;
        case CullMode::Back: return MTL::CullModeBack;
    }
}

}  // namespace radray::render::metal

namespace MTL {
std::string_view format_as(LanguageVersion v) noexcept {
    switch (v) {
        case LanguageVersion1_0: return "1.0";
        case LanguageVersion1_1: return "1.1";
        case LanguageVersion1_2: return "1.2";
        case LanguageVersion2_0: return "2.0";
        case LanguageVersion2_1: return "2.1";
        case LanguageVersion2_2: return "2.2";
        case LanguageVersion2_3: return "2.3";
        case LanguageVersion2_4: return "2.4";
        case LanguageVersion3_0: return "3.0";
        case LanguageVersion3_1: return "3.1";
        case LanguageVersion3_2: return "3.2";
    }
}
std::string_view format_as(GPUFamily v) noexcept {
    switch (v) {
        case GPUFamilyApple1: return "Apple1";
        case GPUFamilyApple2: return "Apple2";
        case GPUFamilyApple3: return "Apple3";
        case GPUFamilyApple4: return "Apple4";
        case GPUFamilyApple5: return "Apple5";
        case GPUFamilyApple6: return "Apple6";
        case GPUFamilyApple7: return "Apple7";
        case GPUFamilyApple8: return "Apple8";
        case GPUFamilyApple9: return "Apple9";
        case GPUFamilyMac1: return "Mac1";
        case GPUFamilyMac2: return "Mac2";
        case GPUFamilyCommon1: return "Common1";
        case GPUFamilyCommon2: return "Common2";
        case GPUFamilyCommon3: return "Common3";
        case GPUFamilyMacCatalyst1: return "MacCatalyst1";
        case GPUFamilyMacCatalyst2: return "MacCatalyst2";
        case GPUFamilyMetal3: return "Metal3";
    }
}
}  // namespace MTL
