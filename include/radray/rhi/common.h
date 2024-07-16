#pragma once

#include <optional>

#include <radray/logger.h>

namespace radray::rhi {

enum class ApiType {
    D3D12,
    Metal,
    MAX_COUNT
};

enum class CommandListType {
    Graphics,
    Compute
};

enum class BufferType {
    Default,
    Upload,
    Readback
};

enum class PixelFormat : uint32_t {
    Unknown,
    R8_SInt,
    R8_UInt,
    R8_UNorm,
    RG8_SInt,
    RG8_UInt,
    RG8_UNorm,
    RGBA8_SInt,
    RGBA8_UInt,
    RGBA8_UNorm,
    R16_SInt,
    R16_UInt,
    R16_UNorm,
    RG16_SInt,
    RG16_UInt,
    RG16_UNorm,
    RGBA16_SInt,
    RGBA16_UInt,
    RGBA16_UNorm,
    R32_SInt,
    R32_UInt,
    RG32_SInt,
    RG32_UInt,
    RGBA32_SInt,
    RGBA32_UInt,
    R16_Float,
    RG16_Float,
    RGBA16_Float,
    R32_Float,
    RG32_Float,
    RGBA32_Float,
    R10G10B10A2_UInt,
    R10G10B10A2_UNorm,
    R11G11B10_Float,

    D16_UNorm,
    D32_Float,
    D24S8,
    D32S8
};

enum class TextureDimension {
    Tex_1D,
    Tex_2D,
    Tex_3D,
    Cubemap,
    Tex_2D_Array
};

enum class PrimitiveTopology {
    Point_List,
    Line_List,
    Line_Strip,
    Triangle_List,
    Triangle_Strip
};

enum class BlendType {
    Zero,
    One,
    Src_Color,
    Inv_Src_Color,
    Src_Alpha,
    Inv_Src_Alpha,
    Dest_Alpha,
    Inv_Dest_Alpha,
    Dest_Color,
    Inv_Dest_Color,
    Src_Alpha_Sat,
    Blend_Factor,
    Inv_Blend_Factor,
    Src1_Color,
    Inv_Src1_Color,
    Src1_Alpha,
    Inv_Src1_Alpha,
    Alpha_Factor,
    Inv_Alpha_Factor
};

enum class BlendOpMode {
    Add,
    Subtract,
    Rev_Subtract,
    Min,
    Max
};

enum class LogicOpMode {
    Clear,
    Set,
    Copy,
    Copy_Inverted,
    Noop,
    Invert,
    And,
    Nand,
    Or,
    Nor,
    Xor,
    Equiv,
    And_Reverse,
    And_Inverted,
    Or_Reverse,
    Or_Inverted
};

enum class FillMode {
    Solid,
    Wireframe
};

enum class CullMode {
    None,
    Front,
    Back
};

enum class LineRasterizationMode {
    Aliased,
    Alpha_Antialiased,
    Quadrilateral_Wide,
    Quadrilateral_Narrow
};

enum class ConservativeRasterizationMode {
    Off,
    On
};

enum class DepthWriteMask {
    Zero,
    All
};

enum class ComparisonFunc {
    None,
    Never,
    Less,
    Equal,
    Less_Equal,
    Greater,
    Not_Equal,
    Greater_Equal,
    Always
};

enum class StencilOpType {
    Keep,
    Zero,
    Replace,
    Incr_Sat,
    Decr_Sat,
    Invert,
    Incr,
    Decr
};

enum class SemanticType {
    Position,
    Normal,
    Texcoord,
    Tanget,
    Color,
    Psize,
    Bi_Nomral,
    Blend_Indices,
    Blend_Weight,
    Position_T,
};

enum class InputClassification {
    Vertex,
    Instance
};

enum class InputElementFormat {
    Float,
    Float2,
    Float3,
    Float4,
    Int,
    Int2,
    Int3,
    Int4,
    UInt,
    UInt2,
    UInt3,
    UInt4,
};

enum class ColorWriteEnable : uint8_t {
    Red = 1,
    Green = 2,
    Blue = 4,
    Alpha = 8,
    All = (((Red | Green) | Blue) | Alpha)
};

struct DeviceCreateInfoD3D12 {
    std::optional<uint32_t> AdapterIndex;
    bool IsEnableDebugLayer;
};

struct DeviceCreateInfoMetal {
    std::optional<uint32_t> DeviceIndex;
};

struct SwapChainCreateInfo {
    uint64_t WindowHandle;
    uint32_t Width;
    uint32_t Height;
    uint32_t BackBufferCount;
    bool Vsync;
};

struct RenderTargetBlendInfo {
    BlendType SrcBlend;
    BlendType DestBlend;
    BlendOpMode BlendOp;
    BlendType SrcBlendAlpha;
    BlendType DestBlendAlpha;
    BlendOpMode BlendOpAlpha;
    LogicOpMode LogicOp;
    ColorWriteEnable RenderTargetWriteMask;
    bool BlendEnable;
    bool LogicOpEnable;
};

struct RasterizerInfo {
    FillMode Fill;
    CullMode Cull;
    float DepthBias;
    float DepthBiasClamp;
    float SlopeScaledDepthBias;
    LineRasterizationMode LineRaster;
    uint32_t ForcedSampleCount;
    ConservativeRasterizationMode ConservativeRaster;
    bool FrontCounterClockwise;
    bool DepthClipEnable;
};

struct DepthStencilOpInfo {
    StencilOpType StencilFailOp;
    StencilOpType StencilDepthFailOp;
    StencilOpType StencilPassOp;
    ComparisonFunc StencilFunc;
};

struct DepthStencilInfo {
    DepthWriteMask DepthMask;
    ComparisonFunc DepthFunc;
    DepthStencilOpInfo FrontFace;
    DepthStencilOpInfo BackFace;
    uint8_t StencilReadMask;
    uint8_t StencilWriteMask;
    bool DepthEnable;
    bool StencilEnable;
};

struct InputElementInfo {
    SemanticType Semantic;
    uint32_t SemanticIndex;
    InputElementFormat Format;
    uint32_t Slot;
    uint32_t ByteOffset;
    InputClassification Class;
    uint32_t StepRate;
};

struct GraphicsPipelineStateInfo {
    RasterizerInfo Raster;
    DepthStencilInfo DepthStencil;
    PrimitiveTopology Topology;
    uint32_t RtCount;
    PixelFormat DsvFormat;
    PixelFormat RtvFormats[8];
    RenderTargetBlendInfo BlendStates[8];
    bool AlphaToCoverageEnable;
};

struct ShaderBlob {
    const void* const Data;
    size_t Length;
};

struct GraphicsShaderInfo {
    ShaderBlob Vs;
    ShaderBlob Ps;
};

const char* to_string(ApiType val) noexcept;
const char* to_string(PixelFormat val) noexcept;
const char* to_string(TextureDimension val) noexcept;
const char* to_string(BufferType val) noexcept;
const char* to_string(PrimitiveTopology val) noexcept;
const char* to_string(BlendType val) noexcept;
const char* to_string(BlendOpMode val) noexcept;
const char* to_string(LogicOpMode val) noexcept;
const char* to_string(FillMode val) noexcept;
const char* to_string(CullMode val) noexcept;
const char* to_string(LineRasterizationMode val) noexcept;
const char* to_string(ConservativeRasterizationMode val) noexcept;
const char* to_string(DepthWriteMask val) noexcept;
const char* to_string(ComparisonFunc val) noexcept;
const char* to_string(StencilOpType val) noexcept;
const char* to_string(SemanticType val) noexcept;
const char* to_string(InputClassification val) noexcept;
const char* to_string(InputElementFormat val) noexcept;
const char* to_string(ColorWriteEnable val) noexcept;

}  // namespace radray::rhi

template <class CharT>
struct std::formatter<radray::rhi::ApiType, CharT> : std::formatter<const char*, CharT> {
    template <class FormatContext>
    auto format(radray::rhi::ApiType val, FormatContext& ctx) const {
        return formatter<const char*, CharT>::format(to_string(val), ctx);
    }
};

template <class CharT>
struct std::formatter<radray::rhi::PixelFormat, CharT> : std::formatter<const char*, CharT> {
    template <class FormatContext>
    auto format(radray::rhi::PixelFormat val, FormatContext& ctx) const {
        return formatter<const char*, CharT>::format(to_string(val), ctx);
    }
};

template <class CharT>
struct std::formatter<radray::rhi::TextureDimension, CharT> : std::formatter<const char*, CharT> {
    template <class FormatContext>
    auto format(radray::rhi::TextureDimension val, FormatContext& ctx) const {
        return formatter<const char*, CharT>::format(to_string(val), ctx);
    }
};

template <class CharT>
struct std::formatter<radray::rhi::BufferType, CharT> : std::formatter<const char*, CharT> {
    template <class FormatContext>
    auto format(radray::rhi::BufferType val, FormatContext& ctx) const {
        return formatter<const char*, CharT>::format(to_string(val), ctx);
    }
};

template <class CharT>
struct std::formatter<radray::rhi::PrimitiveTopology, CharT> : std::formatter<const char*, CharT> {
    template <class FormatContext>
    auto format(radray::rhi::PrimitiveTopology val, FormatContext& ctx) const {
        return formatter<const char*, CharT>::format(to_string(val), ctx);
    }
};

template <class CharT>
struct std::formatter<radray::rhi::BlendType, CharT> : std::formatter<const char*, CharT> {
    template <class FormatContext>
    auto format(radray::rhi::BlendType val, FormatContext& ctx) const {
        return formatter<const char*, CharT>::format(to_string(val), ctx);
    }
};

template <class CharT>
struct std::formatter<radray::rhi::BlendOpMode, CharT> : std::formatter<const char*, CharT> {
    template <class FormatContext>
    auto format(radray::rhi::BlendOpMode val, FormatContext& ctx) const {
        return formatter<const char*, CharT>::format(to_string(val), ctx);
    }
};

template <class CharT>
struct std::formatter<radray::rhi::LogicOpMode, CharT> : std::formatter<const char*, CharT> {
    template <class FormatContext>
    auto format(radray::rhi::LogicOpMode val, FormatContext& ctx) const {
        return formatter<const char*, CharT>::format(to_string(val), ctx);
    }
};

template <class CharT>
struct std::formatter<radray::rhi::FillMode, CharT> : std::formatter<const char*, CharT> {
    template <class FormatContext>
    auto format(radray::rhi::FillMode val, FormatContext& ctx) const {
        return formatter<const char*, CharT>::format(to_string(val), ctx);
    }
};

template <class CharT>
struct std::formatter<radray::rhi::CullMode, CharT> : std::formatter<const char*, CharT> {
    template <class FormatContext>
    auto format(radray::rhi::CullMode val, FormatContext& ctx) const {
        return formatter<const char*, CharT>::format(to_string(val), ctx);
    }
};

template <class CharT>
struct std::formatter<radray::rhi::LineRasterizationMode, CharT> : std::formatter<const char*, CharT> {
    template <class FormatContext>
    auto format(radray::rhi::LineRasterizationMode val, FormatContext& ctx) const {
        return formatter<const char*, CharT>::format(to_string(val), ctx);
    }
};

template <class CharT>
struct std::formatter<radray::rhi::ConservativeRasterizationMode, CharT> : std::formatter<const char*, CharT> {
    template <class FormatContext>
    auto format(radray::rhi::ConservativeRasterizationMode val, FormatContext& ctx) const {
        return formatter<const char*, CharT>::format(to_string(val), ctx);
    }
};

template <class CharT>
struct std::formatter<radray::rhi::DepthWriteMask, CharT> : std::formatter<const char*, CharT> {
    template <class FormatContext>
    auto format(radray::rhi::DepthWriteMask val, FormatContext& ctx) const {
        return formatter<const char*, CharT>::format(to_string(val), ctx);
    }
};

template <class CharT>
struct std::formatter<radray::rhi::ComparisonFunc, CharT> : std::formatter<const char*, CharT> {
    template <class FormatContext>
    auto format(radray::rhi::ComparisonFunc val, FormatContext& ctx) const {
        return formatter<const char*, CharT>::format(to_string(val), ctx);
    }
};

template <class CharT>
struct std::formatter<radray::rhi::StencilOpType, CharT> : std::formatter<const char*, CharT> {
    template <class FormatContext>
    auto format(radray::rhi::StencilOpType val, FormatContext& ctx) const {
        return formatter<const char*, CharT>::format(to_string(val), ctx);
    }
};

template <class CharT>
struct std::formatter<radray::rhi::SemanticType, CharT> : std::formatter<const char*, CharT> {
    template <class FormatContext>
    auto format(radray::rhi::SemanticType val, FormatContext& ctx) const {
        return formatter<const char*, CharT>::format(to_string(val), ctx);
    }
};

template <class CharT>
struct std::formatter<radray::rhi::InputClassification, CharT> : std::formatter<const char*, CharT> {
    template <class FormatContext>
    auto format(radray::rhi::InputClassification val, FormatContext& ctx) const {
        return formatter<const char*, CharT>::format(to_string(val), ctx);
    }
};

template <class CharT>
struct std::formatter<radray::rhi::InputElementFormat, CharT> : std::formatter<const char*, CharT> {
    template <class FormatContext>
    auto format(radray::rhi::InputElementFormat val, FormatContext& ctx) const {
        return formatter<const char*, CharT>::format(to_string(val), ctx);
    }
};

template <class CharT>
struct std::formatter<radray::rhi::ColorWriteEnable, CharT> : std::formatter<const char*, CharT> {
    template <class FormatContext>
    auto format(radray::rhi::ColorWriteEnable val, FormatContext& ctx) const {
        return formatter<const char*, CharT>::format(to_string(val), ctx);
    }
};
