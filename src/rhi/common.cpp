#include <radray/rhi/common.h>

namespace radray::rhi {

const char* to_string(ApiType val) noexcept {
    switch (val) {
        case radray::rhi::ApiType::D3D12: return "D3D12";
        case radray::rhi::ApiType::Metal: return "Metal";
        case radray::rhi::ApiType::MAX_COUNT: return "Unknown";
        default: return "Unknown";
    }
}

const char* to_string(PixelFormat val) noexcept {
    switch (val) {
        case PixelFormat::Unknown: return "Unknown";
        case PixelFormat::R8_SInt: return "R8_SInt";
        case PixelFormat::R8_UInt: return "R8_UInt";
        case PixelFormat::R8_UNorm: return "R8_UNorm";
        case PixelFormat::RG8_SInt: return "RG8_SInt";
        case PixelFormat::RG8_UInt: return "RG8_UInt";
        case PixelFormat::RG8_UNorm: return "RG8_UNorm";
        case PixelFormat::RGBA8_SInt: return "RGBA8_SInt";
        case PixelFormat::RGBA8_UInt: return "RGBA8_UInt";
        case PixelFormat::RGBA8_UNorm: return "RGBA8_UNorm";
        case PixelFormat::R16_SInt: return "R16_SInt";
        case PixelFormat::R16_UInt: return "R16_UInt";
        case PixelFormat::R16_UNorm: return "R16_UNorm";
        case PixelFormat::RG16_SInt: return "RG16_SInt";
        case PixelFormat::RG16_UInt: return "RG16_UInt";
        case PixelFormat::RG16_UNorm: return "RG16_UNorm";
        case PixelFormat::RGBA16_SInt: return "RGBA16_SInt";
        case PixelFormat::RGBA16_UInt: return "RGBA16_UInt";
        case PixelFormat::RGBA16_UNorm: return "RGBA16_UNorm";
        case PixelFormat::R32_SInt: return "R32_SInt";
        case PixelFormat::R32_UInt: return "R32_UInt";
        case PixelFormat::RG32_SInt: return "RG32_SInt";
        case PixelFormat::RG32_UInt: return "RG32_UInt";
        case PixelFormat::RGBA32_SInt: return "RGBA32_SInt";
        case PixelFormat::RGBA32_UInt: return "RGBA32_UInt";
        case PixelFormat::R16_Float: return "R16_Float";
        case PixelFormat::RG16_Float: return "RG16_Float";
        case PixelFormat::RGBA16_Float: return "RGBA16_Float";
        case PixelFormat::R32_Float: return "R32_Float";
        case PixelFormat::RG32_Float: return "RG32_Float";
        case PixelFormat::RGBA32_Float: return "RGBA32_Float";
        case PixelFormat::R10G10B10A2_UInt: return "R10G10B10A2_UInt";
        case PixelFormat::R10G10B10A2_UNorm: return "R10G10B10A2_UNorm";
        case PixelFormat::R11G11B10_Float: return "R11G11B10_Float";
        case PixelFormat::D16_UNorm: return "D16_UNorm";
        case PixelFormat::D32_Float: return "D32_Float";
        case PixelFormat::D24S8: return "D24S8";
        case PixelFormat::D32S8: return "D32S8";
        default: return "Unknown";
    }
}

const char* to_string(TextureDimension val) noexcept {
    switch (val) {
        case TextureDimension::Tex_1D: return "Tex_1D";
        case TextureDimension::Tex_2D: return "Tex_2D";
        case TextureDimension::Tex_3D: return "Tex_3D";
        case TextureDimension::Cubemap: return "Cubemap";
        case TextureDimension::Tex_2D_Array: return "Tex_2D_Array";
        default: return "Unknown";
    }
}

const char* to_string(BufferType val) noexcept {
    switch (val) {
        case BufferType::Default: return "Default";
        case BufferType::Upload: return "Upload";
        case BufferType::Readback: return "Readback";
        default: return "Unknown";
    }
}

const char* to_string(PrimitiveTopology val) noexcept {
    switch (val) {
        case PrimitiveTopology::Point_List: return "Point_List";
        case PrimitiveTopology::Line_List: return "Line_List";
        case PrimitiveTopology::Line_Strip: return "Line_Strip";
        case PrimitiveTopology::Triangle_List: return "Triangle_List";
        case PrimitiveTopology::Triangle_Strip: return "Triangle_Strip";
        default: return "Unknown";
    }
}

const char* to_string(BlendType val) noexcept {
    switch (val) {
        case BlendType::Zero: return "Zero";
        case BlendType::One: return "One";
        case BlendType::Src_Color: return "Src_Color";
        case BlendType::Inv_Src_Color: return "Inv_Src_Color";
        case BlendType::Src_Alpha: return "Src_Alpha";
        case BlendType::Inv_Src_Alpha: return "Inv_Src_Alpha";
        case BlendType::Dest_Alpha: return "Dest_Alpha";
        case BlendType::Inv_Dest_Alpha: return "Inv_Dest_Alpha";
        case BlendType::Dest_Color: return "Dest_Color";
        case BlendType::Inv_Dest_Color: return "Inv_Dest_Color";
        case BlendType::Src_Alpha_Sat: return "Src_Alpha_Sat";
        case BlendType::Blend_Factor: return "Blend_Factor";
        case BlendType::Inv_Blend_Factor: return "Inv_Blend_Factor";
        case BlendType::Src1_Color: return "Src1_Color";
        case BlendType::Inv_Src1_Color: return "Inv_Src1_Color";
        case BlendType::Src1_Alpha: return "Src1_Alpha";
        case BlendType::Inv_Src1_Alpha: return "Inv_Src1_Alpha";
        case BlendType::Alpha_Factor: return "Alpha_Factor";
        case BlendType::Inv_Alpha_Factor: return "Inv_Alpha_Factor";
        default: return "Unknown";
    }
}

const char* to_string(BlendOpMode val) noexcept {
    switch (val) {
        case BlendOpMode::Add: return "Add";
        case BlendOpMode::Subtract: return "Subtract";
        case BlendOpMode::Rev_Subtract: return "Rev_Subtract";
        case BlendOpMode::Min: return "Min";
        case BlendOpMode::Max: return "Max";
        default: return "Unknown";
    }
}

const char* to_string(LogicOpMode val) noexcept {
    switch (val) {
        case LogicOpMode::Clear: return "Clear";
        case LogicOpMode::Set: return "Set";
        case LogicOpMode::Copy: return "Copy";
        case LogicOpMode::Copy_Inverted: return "Copy_Inverted";
        case LogicOpMode::Noop: return "Noop";
        case LogicOpMode::Invert: return "Invert";
        case LogicOpMode::And: return "And";
        case LogicOpMode::Nand: return "Nand";
        case LogicOpMode::Or: return "Or";
        case LogicOpMode::Nor: return "Nor";
        case LogicOpMode::Xor: return "Xor";
        case LogicOpMode::Equiv: return "Equiv";
        case LogicOpMode::And_Reverse: return "And_Reverse";
        case LogicOpMode::And_Inverted: return "And_Inverted";
        case LogicOpMode::Or_Reverse: return "Or_Reverse";
        case LogicOpMode::Or_Inverted: return "Or_Inverted";
        default: return "Unknown";
    }
}

const char* to_string(FillMode val) noexcept {
    switch (val) {
        case FillMode::Solid: return "Solid";
        case FillMode::Wireframe: return "Wireframe";
        default: return "Unknown";
    }
}

const char* to_string(CullMode val) noexcept {
    switch (val) {
        case CullMode::None: return "None";
        case CullMode::Front: return "Front";
        case CullMode::Back: return "Back";
        default: return "Unknown";
    }
}

const char* to_string(LineRasterizationMode val) noexcept {
    switch (val) {
        case LineRasterizationMode::Aliased: return "Aliased";
        case LineRasterizationMode::Alpha_Antialiased: return "Alpha_Antialiased";
        case LineRasterizationMode::Quadrilateral_Wide: return "Quadrilateral_Wide";
        case LineRasterizationMode::Quadrilateral_Narrow: return "Quadrilateral_Narrow";
        default: return "Unknown";
    }
}

const char* to_string(ConservativeRasterizationMode val) noexcept {
    switch (val) {
        case ConservativeRasterizationMode::Off: return "Off";
        case ConservativeRasterizationMode::On: return "On";
        default: return "Unknown";
    }
}

const char* to_string(DepthWriteMask val) noexcept {
    switch (val) {
        case DepthWriteMask::Zero: return "Zero";
        case DepthWriteMask::All: return "All";
        default: return "Unknown";
    }
}

const char* to_string(ComparisonFunc val) noexcept {
    switch (val) {
        case ComparisonFunc::None: return "None";
        case ComparisonFunc::Never: return "Never";
        case ComparisonFunc::Less: return "Less";
        case ComparisonFunc::Equal: return "Equal";
        case ComparisonFunc::Less_Equal: return "Less_Equal";
        case ComparisonFunc::Greater: return "Greater";
        case ComparisonFunc::Not_Equal: return "Not_Equal";
        case ComparisonFunc::Greater_Equal: return "Greater_Equal";
        case ComparisonFunc::Always: return "Always";
        default: return "Unknown";
    }
}

const char* to_string(StencilOpType val) noexcept {
    switch (val) {
        case StencilOpType::Keep: return "Keep";
        case StencilOpType::Zero: return "Zero";
        case StencilOpType::Replace: return "Replace";
        case StencilOpType::Incr_Sat: return "Incr_Sat";
        case StencilOpType::Decr_Sat: return "Decr_Sat";
        case StencilOpType::Invert: return "Invert";
        case StencilOpType::Incr: return "Incr";
        case StencilOpType::Decr: return "Decr";
        default: return "Unknown";
    }
}

const char* to_string(SemanticType val) noexcept {
    switch (val) {
        case SemanticType::Position: return "Position";
        case SemanticType::Normal: return "Normal";
        case SemanticType::Texcoord: return "Texcoord";
        case SemanticType::Tanget: return "Tanget";
        case SemanticType::Color: return "Color";
        case SemanticType::Psize: return "Psize";
        case SemanticType::Bi_Nomral: return "Bi_Nomral";
        case SemanticType::Blend_Indices: return "Blend_Indices";
        case SemanticType::Blend_Weight: return "Blend_Weight";
        case SemanticType::Position_T: return "Position_T";
        default: return "Unknown";
    }
}

const char* to_string(InputClassification val) noexcept {
    switch (val) {
        case InputClassification::Vertex: return "Vertex";
        case InputClassification::Instance: return "Instance";
        default: return "Unknown";
    }
}

const char* to_string(InputElementFormat val) noexcept {
    switch (val) {
        case InputElementFormat::Float: return "Float";
        case InputElementFormat::Float2: return "Float2";
        case InputElementFormat::Float3: return "Float3";
        case InputElementFormat::Float4: return "Float4";
        case InputElementFormat::Int: return "Int";
        case InputElementFormat::Int2: return "Int2";
        case InputElementFormat::Int3: return "Int3";
        case InputElementFormat::Int4: return "Int4";
        case InputElementFormat::UInt: return "UInt";
        case InputElementFormat::UInt2: return "UInt2";
        case InputElementFormat::UInt3: return "UInt3";
        case InputElementFormat::UInt4: return "UInt4";
        default: return "Unknown";
    }
}

const char* to_string(ColorWriteEnable val) noexcept {
    switch (val) {
        case ColorWriteEnable::Red: return "Red";
        case ColorWriteEnable::Green: return "Green";
        case ColorWriteEnable::Blue: return "Blue";
        case ColorWriteEnable::Alpha: return "Alpha";
        case ColorWriteEnable::All: return "All";
        default: return "Unknown";
    }
}

}  // namespace radray::rhi
