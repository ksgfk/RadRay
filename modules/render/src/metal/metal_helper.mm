#include <radray/render/backend/metal_helper.h>

#include <radray/utility.h>

namespace radray::render::metal {

MTLPixelFormat MapPixelFormat(TextureFormat v) noexcept {
    switch (v) {
        case TextureFormat::UNKNOWN: return MTLPixelFormatInvalid;
        case TextureFormat::R8_SINT: return MTLPixelFormatR8Sint;
        case TextureFormat::R8_UINT: return MTLPixelFormatR8Uint;
        case TextureFormat::R8_SNORM: return MTLPixelFormatR8Snorm;
        case TextureFormat::R8_UNORM: return MTLPixelFormatR8Unorm;
        case TextureFormat::R16_SINT: return MTLPixelFormatR16Sint;
        case TextureFormat::R16_UINT: return MTLPixelFormatR16Uint;
        case TextureFormat::R16_SNORM: return MTLPixelFormatR16Snorm;
        case TextureFormat::R16_UNORM: return MTLPixelFormatR16Unorm;
        case TextureFormat::R16_FLOAT: return MTLPixelFormatR16Float;
        case TextureFormat::RG8_SINT: return MTLPixelFormatRG8Sint;
        case TextureFormat::RG8_UINT: return MTLPixelFormatRG8Uint;
        case TextureFormat::RG8_SNORM: return MTLPixelFormatRG8Snorm;
        case TextureFormat::RG8_UNORM: return MTLPixelFormatRG8Unorm;
        case TextureFormat::R32_SINT: return MTLPixelFormatR32Sint;
        case TextureFormat::R32_UINT: return MTLPixelFormatR32Uint;
        case TextureFormat::R32_FLOAT: return MTLPixelFormatR32Float;
        case TextureFormat::RG16_SINT: return MTLPixelFormatRG16Sint;
        case TextureFormat::RG16_UINT: return MTLPixelFormatRG16Uint;
        case TextureFormat::RG16_SNORM: return MTLPixelFormatRG16Snorm;
        case TextureFormat::RG16_UNORM: return MTLPixelFormatRG16Unorm;
        case TextureFormat::RG16_FLOAT: return MTLPixelFormatRG16Float;
        case TextureFormat::RGBA8_SINT: return MTLPixelFormatRGBA8Sint;
        case TextureFormat::RGBA8_UINT: return MTLPixelFormatRGBA8Uint;
        case TextureFormat::RGBA8_SNORM: return MTLPixelFormatRGBA8Snorm;
        case TextureFormat::RGBA8_UNORM: return MTLPixelFormatRGBA8Unorm;
        case TextureFormat::RGBA8_UNORM_SRGB: return MTLPixelFormatRGBA8Unorm_sRGB;
        case TextureFormat::BGRA8_UNORM: return MTLPixelFormatBGRA8Unorm;
        case TextureFormat::BGRA8_UNORM_SRGB: return MTLPixelFormatBGRA8Unorm_sRGB;
        case TextureFormat::RGB10A2_UINT: return MTLPixelFormatRGB10A2Uint;
        case TextureFormat::RGB10A2_UNORM: return MTLPixelFormatRGB10A2Unorm;
        case TextureFormat::RG11B10_FLOAT: return MTLPixelFormatRG11B10Float;
        case TextureFormat::RG32_SINT: return MTLPixelFormatRG32Sint;
        case TextureFormat::RG32_UINT: return MTLPixelFormatRG32Uint;
        case TextureFormat::RG32_FLOAT: return MTLPixelFormatRG32Float;
        case TextureFormat::RGBA16_SINT: return MTLPixelFormatRGBA16Sint;
        case TextureFormat::RGBA16_UINT: return MTLPixelFormatRGBA16Uint;
        case TextureFormat::RGBA16_SNORM: return MTLPixelFormatRGBA16Snorm;
        case TextureFormat::RGBA16_UNORM: return MTLPixelFormatRGBA16Unorm;
        case TextureFormat::RGBA16_FLOAT: return MTLPixelFormatRGBA16Float;
        case TextureFormat::RGBA32_SINT: return MTLPixelFormatRGBA32Sint;
        case TextureFormat::RGBA32_UINT: return MTLPixelFormatRGBA32Uint;
        case TextureFormat::RGBA32_FLOAT: return MTLPixelFormatRGBA32Float;
        case TextureFormat::S8: return MTLPixelFormatStencil8;
        case TextureFormat::D16_UNORM: return MTLPixelFormatDepth16Unorm;
        case TextureFormat::D32_FLOAT: return MTLPixelFormatDepth32Float;
#if defined(RADRAY_PLATFORM_MACOS)
        case TextureFormat::D24_UNORM_S8_UINT: return MTLPixelFormatDepth24Unorm_Stencil8;
#else
        case TextureFormat::D24_UNORM_S8_UINT: return MTLPixelFormatInvalid;
#endif
        case TextureFormat::D32_FLOAT_S8_UINT: return MTLPixelFormatDepth32Float_Stencil8;
    }
    return MTLPixelFormatInvalid;
}

MTLLoadAction MapLoadAction(LoadAction v) noexcept {
    switch (v) {
        case LoadAction::DontCare: return MTLLoadActionDontCare;
        case LoadAction::Load: return MTLLoadActionLoad;
        case LoadAction::Clear: return MTLLoadActionClear;
    }
    return MTLLoadActionDontCare;
}

MTLStoreAction MapStoreAction(StoreAction v) noexcept {
    switch (v) {
        case StoreAction::Store: return MTLStoreActionStore;
        case StoreAction::Discard: return MTLStoreActionDontCare;
    }
    return MTLStoreActionStore;
}

MTLTextureType MapTextureType(TextureDimension dim, uint32_t sampleCount) noexcept {
    switch (dim) {
        case TextureDimension::Dim1D: return MTLTextureType1D;
        case TextureDimension::Dim2D: return sampleCount > 1 ? MTLTextureType2DMultisample : MTLTextureType2D;
        case TextureDimension::Dim3D: return MTLTextureType3D;
        case TextureDimension::UNKNOWN: return MTLTextureType2D;
    }
    return MTLTextureType2D;
}

MTLResourceOptions MapResourceOptions(MemoryType mem) noexcept {
    switch (mem) {
        case MemoryType::Device: return MTLResourceStorageModePrivate;
        case MemoryType::Upload: return MTLResourceStorageModeShared | MTLResourceCPUCacheModeWriteCombined;
        case MemoryType::ReadBack: return MTLResourceStorageModeShared;
    }
    return MTLResourceStorageModePrivate;
}

MTLStorageMode MapStorageMode(MemoryType mem) noexcept {
    switch (mem) {
        case MemoryType::Device: return MTLStorageModePrivate;
        case MemoryType::Upload: return MTLStorageModeShared;
        case MemoryType::ReadBack: return MTLStorageModeShared;
    }
    return MTLStorageModePrivate;
}

MTLTextureType MapTextureViewType(TextureViewDimension dim) noexcept {
    switch (dim) {
        case TextureViewDimension::Dim1D: return MTLTextureType1D;
        case TextureViewDimension::Dim2D: return MTLTextureType2D;
        case TextureViewDimension::Dim3D: return MTLTextureType3D;
        case TextureViewDimension::Dim1DArray: return MTLTextureType1DArray;
        case TextureViewDimension::Dim2DArray: return MTLTextureType2DArray;
        case TextureViewDimension::Cube: return MTLTextureTypeCube;
        case TextureViewDimension::CubeArray: return MTLTextureTypeCubeArray;
        case TextureViewDimension::UNKNOWN: return MTLTextureType2D;
    }
    return MTLTextureType2D;
}

MTLCompareFunction MapCompareFunction(CompareFunction v) noexcept {
    switch (v) {
        case CompareFunction::Never: return MTLCompareFunctionNever;
        case CompareFunction::Less: return MTLCompareFunctionLess;
        case CompareFunction::Equal: return MTLCompareFunctionEqual;
        case CompareFunction::LessEqual: return MTLCompareFunctionLessEqual;
        case CompareFunction::Greater: return MTLCompareFunctionGreater;
        case CompareFunction::NotEqual: return MTLCompareFunctionNotEqual;
        case CompareFunction::GreaterEqual: return MTLCompareFunctionGreaterEqual;
        case CompareFunction::Always: return MTLCompareFunctionAlways;
    }
    return MTLCompareFunctionAlways;
}

MTLSamplerAddressMode MapAddressMode(AddressMode v) noexcept {
    switch (v) {
        case AddressMode::ClampToEdge: return MTLSamplerAddressModeClampToEdge;
        case AddressMode::Repeat: return MTLSamplerAddressModeRepeat;
        case AddressMode::Mirror: return MTLSamplerAddressModeMirrorRepeat;
    }
    return MTLSamplerAddressModeClampToEdge;
}

MTLSamplerMinMagFilter MapMinMagFilter(FilterMode v) noexcept {
    switch (v) {
        case FilterMode::Nearest: return MTLSamplerMinMagFilterNearest;
        case FilterMode::Linear: return MTLSamplerMinMagFilterLinear;
    }
    return MTLSamplerMinMagFilterNearest;
}

MTLSamplerMipFilter MapMipFilter(FilterMode v) noexcept {
    switch (v) {
        case FilterMode::Nearest: return MTLSamplerMipFilterNearest;
        case FilterMode::Linear: return MTLSamplerMipFilterLinear;
    }
    return MTLSamplerMipFilterNearest;
}

MTLBlendFactor MapBlendFactor(BlendFactor v) noexcept {
    switch (v) {
        case BlendFactor::Zero: return MTLBlendFactorZero;
        case BlendFactor::One: return MTLBlendFactorOne;
        case BlendFactor::Src: return MTLBlendFactorSourceColor;
        case BlendFactor::OneMinusSrc: return MTLBlendFactorOneMinusSourceColor;
        case BlendFactor::SrcAlpha: return MTLBlendFactorSourceAlpha;
        case BlendFactor::OneMinusSrcAlpha: return MTLBlendFactorOneMinusSourceAlpha;
        case BlendFactor::Dst: return MTLBlendFactorDestinationColor;
        case BlendFactor::OneMinusDst: return MTLBlendFactorOneMinusDestinationColor;
        case BlendFactor::DstAlpha: return MTLBlendFactorDestinationAlpha;
        case BlendFactor::OneMinusDstAlpha: return MTLBlendFactorOneMinusDestinationAlpha;
        case BlendFactor::SrcAlphaSaturated: return MTLBlendFactorSourceAlphaSaturated;
        case BlendFactor::Constant: return MTLBlendFactorBlendColor;
        case BlendFactor::OneMinusConstant: return MTLBlendFactorOneMinusBlendColor;
        case BlendFactor::Src1: return MTLBlendFactorSource1Color;
        case BlendFactor::OneMinusSrc1: return MTLBlendFactorOneMinusSource1Color;
        case BlendFactor::Src1Alpha: return MTLBlendFactorSource1Alpha;
        case BlendFactor::OneMinusSrc1Alpha: return MTLBlendFactorOneMinusSource1Alpha;
    }
    return MTLBlendFactorZero;
}

MTLBlendOperation MapBlendOperation(BlendOperation v) noexcept {
    switch (v) {
        case BlendOperation::Add: return MTLBlendOperationAdd;
        case BlendOperation::Subtract: return MTLBlendOperationSubtract;
        case BlendOperation::ReverseSubtract: return MTLBlendOperationReverseSubtract;
        case BlendOperation::Min: return MTLBlendOperationMin;
        case BlendOperation::Max: return MTLBlendOperationMax;
    }
    return MTLBlendOperationAdd;
}

MTLStencilOperation MapStencilOp(StencilOperation v) noexcept {
    switch (v) {
        case StencilOperation::Keep: return MTLStencilOperationKeep;
        case StencilOperation::Zero: return MTLStencilOperationZero;
        case StencilOperation::Replace: return MTLStencilOperationReplace;
        case StencilOperation::Invert: return MTLStencilOperationInvert;
        case StencilOperation::IncrementClamp: return MTLStencilOperationIncrementClamp;
        case StencilOperation::DecrementClamp: return MTLStencilOperationDecrementClamp;
        case StencilOperation::IncrementWrap: return MTLStencilOperationIncrementWrap;
        case StencilOperation::DecrementWrap: return MTLStencilOperationDecrementWrap;
    }
    return MTLStencilOperationKeep;
}

MTLVertexFormat MapVertexFormat(VertexFormat v) noexcept {
    switch (v) {
        case VertexFormat::UNKNOWN: return MTLVertexFormatInvalid;
        case VertexFormat::UINT8X2: return MTLVertexFormatUChar2;
        case VertexFormat::UINT8X4: return MTLVertexFormatUChar4;
        case VertexFormat::SINT8X2: return MTLVertexFormatChar2;
        case VertexFormat::SINT8X4: return MTLVertexFormatChar4;
        case VertexFormat::UNORM8X2: return MTLVertexFormatUChar2Normalized;
        case VertexFormat::UNORM8X4: return MTLVertexFormatUChar4Normalized;
        case VertexFormat::SNORM8X2: return MTLVertexFormatChar2Normalized;
        case VertexFormat::SNORM8X4: return MTLVertexFormatChar4Normalized;
        case VertexFormat::UINT16X2: return MTLVertexFormatUShort2;
        case VertexFormat::UINT16X4: return MTLVertexFormatUShort4;
        case VertexFormat::SINT16X2: return MTLVertexFormatShort2;
        case VertexFormat::SINT16X4: return MTLVertexFormatShort4;
        case VertexFormat::UNORM16X2: return MTLVertexFormatUShort2Normalized;
        case VertexFormat::UNORM16X4: return MTLVertexFormatUShort4Normalized;
        case VertexFormat::SNORM16X2: return MTLVertexFormatShort2Normalized;
        case VertexFormat::SNORM16X4: return MTLVertexFormatShort4Normalized;
        case VertexFormat::FLOAT16X2: return MTLVertexFormatHalf2;
        case VertexFormat::FLOAT16X4: return MTLVertexFormatHalf4;
        case VertexFormat::UINT32: return MTLVertexFormatUInt;
        case VertexFormat::UINT32X2: return MTLVertexFormatUInt2;
        case VertexFormat::UINT32X3: return MTLVertexFormatUInt3;
        case VertexFormat::UINT32X4: return MTLVertexFormatUInt4;
        case VertexFormat::SINT32: return MTLVertexFormatInt;
        case VertexFormat::SINT32X2: return MTLVertexFormatInt2;
        case VertexFormat::SINT32X3: return MTLVertexFormatInt3;
        case VertexFormat::SINT32X4: return MTLVertexFormatInt4;
        case VertexFormat::FLOAT32: return MTLVertexFormatFloat;
        case VertexFormat::FLOAT32X2: return MTLVertexFormatFloat2;
        case VertexFormat::FLOAT32X3: return MTLVertexFormatFloat3;
        case VertexFormat::FLOAT32X4: return MTLVertexFormatFloat4;
    }
    return MTLVertexFormatInvalid;
}

MTLVertexStepFunction MapVertexStepFunction(VertexStepMode v) noexcept {
    switch (v) {
        case VertexStepMode::Vertex: return MTLVertexStepFunctionPerVertex;
        case VertexStepMode::Instance: return MTLVertexStepFunctionPerInstance;
    }
    return MTLVertexStepFunctionPerVertex;
}

MTLPrimitiveType MapPrimitiveType(PrimitiveTopology v) noexcept {
    switch (v) {
        case PrimitiveTopology::PointList: return MTLPrimitiveTypePoint;
        case PrimitiveTopology::LineList: return MTLPrimitiveTypeLine;
        case PrimitiveTopology::LineStrip: return MTLPrimitiveTypeLineStrip;
        case PrimitiveTopology::TriangleList: return MTLPrimitiveTypeTriangle;
        case PrimitiveTopology::TriangleStrip: return MTLPrimitiveTypeTriangleStrip;
    }
    return MTLPrimitiveTypeTriangle;
}

MTLIndexType MapIndexType(IndexFormat v) noexcept {
    switch (v) {
        case IndexFormat::UINT16: return MTLIndexTypeUInt16;
        case IndexFormat::UINT32: return MTLIndexTypeUInt32;
    }
    return MTLIndexTypeUInt32;
}

MTLCullMode MapCullMode(CullMode v) noexcept {
    switch (v) {
        case CullMode::None: return MTLCullModeNone;
        case CullMode::Front: return MTLCullModeFront;
        case CullMode::Back: return MTLCullModeBack;
    }
    return MTLCullModeNone;
}

MTLWinding MapWinding(FrontFace v) noexcept {
    switch (v) {
        case FrontFace::CCW: return MTLWindingCounterClockwise;
        case FrontFace::CW: return MTLWindingClockwise;
    }
    return MTLWindingClockwise;
}

MTLColorWriteMask MapColorWriteMask(ColorWrites mask) noexcept {
    MTLColorWriteMask result = MTLColorWriteMaskNone;
    if (mask.HasFlag(ColorWrite::Red)) result |= MTLColorWriteMaskRed;
    if (mask.HasFlag(ColorWrite::Green)) result |= MTLColorWriteMaskGreen;
    if (mask.HasFlag(ColorWrite::Blue)) result |= MTLColorWriteMaskBlue;
    if (mask.HasFlag(ColorWrite::Alpha)) result |= MTLColorWriteMaskAlpha;
    return result;
}

ArgumentDescriptorInfo MapResourceBindTypeToArgument(ResourceBindType type) noexcept {
    switch (type) {
        case ResourceBindType::CBuffer:
            return {MTLDataTypePointer, MTLBindingAccessReadOnly};
        case ResourceBindType::Buffer:
            return {MTLDataTypePointer, MTLBindingAccessReadOnly};
        case ResourceBindType::Texture:
            return {MTLDataTypeTexture, MTLBindingAccessReadOnly};
        case ResourceBindType::Sampler:
            return {MTLDataTypeSampler, MTLBindingAccessReadOnly};
        case ResourceBindType::RWBuffer:
            return {MTLDataTypePointer, MTLBindingAccessReadWrite};
        case ResourceBindType::RWTexture:
            return {MTLDataTypeTexture, MTLBindingAccessReadWrite};
        case ResourceBindType::UNKNOWN:
            return {MTLDataTypePointer, MTLBindingAccessReadOnly};
    }
    return {MTLDataTypePointer, MTLBindingAccessReadOnly};
}

}  // namespace radray::render::metal

std::string_view format_as(MTLLanguageVersion v) noexcept {
    switch (v) {
#ifndef __MAC_OS_X_VERSION_MAX_ALLOWED
        case MTLLanguageVersion1_0: return "1.0";
#endif
        case MTLLanguageVersion1_1: return "1.1";
        case MTLLanguageVersion1_2: return "1.2";
        case MTLLanguageVersion2_0: return "2.0";
        case MTLLanguageVersion2_1: return "2.1";
        case MTLLanguageVersion2_2: return "2.2";
        case MTLLanguageVersion2_3: return "2.3";
        case MTLLanguageVersion2_4: return "2.4";
        case MTLLanguageVersion3_0: return "3.0";
        case MTLLanguageVersion3_1: return "3.1";
        case MTLLanguageVersion3_2: return "3.2";
        case MTLLanguageVersion4_0: return "4.0";
    }
    radray::Unreachable();
}
