#pragma once

#include <array>
#include <variant>

#include <radray/types.h>
#include <radray/utility.h>

namespace radray::render {

enum class Backend {
    D3D12,
    Vulkan,
    Metal
};

enum class TextureFormat {
    UNKNOWN,

    R8_SINT,
    R8_UINT,
    R8_SNORM,
    R8_UNORM,

    R16_SINT,
    R16_UINT,
    R16_SNORM,
    R16_UNORM,
    R16_FLOAT,

    RG8_SINT,
    RG8_UINT,
    RG8_SNORM,
    RG8_UNORM,

    R32_SINT,
    R32_UINT,
    R32_FLOAT,

    RG16_SINT,
    RG16_UINT,
    RG16_SNORM,
    RG16_UNORM,
    RG16_FLOAT,

    RGBA8_SINT,
    RGBA8_UINT,
    RGBA8_SNORM,
    RGBA8_UNORM,
    RGBA8_UNORM_SRGB,
    BGRA8_UNORM,
    BGRA8_UNORM_SRGB,

    RGB10A2_UINT,
    RGB10A2_UNORM,
    RG11B10_FLOAT,

    RG32_SINT,
    RG32_UINT,
    RG32_FLOAT,

    RGBA16_SINT,
    RGBA16_UINT,
    RGBA16_SNORM,
    RGBA16_UNORM,
    RGBA16_FLOAT,

    RGBA32_SINT,
    RGBA32_UINT,
    RGBA32_FLOAT,

    S8,
    D16_UNORM,
    D32_FLOAT,
    D24_UNORM_S8_UINT,
    D32_FLOAT_S8_UINT,
};

enum class TextureDimension {
    UNKNOWN,
    Dim1D,
    Dim2D,
    Dim3D,
    Dim1DArray,
    Dim2DArray,
    Cube,
    CubeArray
};

enum class QueueType : uint32_t {
    Direct,
    Compute,
    Copy,
};

enum class ShaderStage : uint32_t {
    UNKNOWN = 0x0,
    Vertex = 0x1,
    Pixel = 0x2,
    Compute = 0x4
};
using ShaderStages = std::underlying_type_t<ShaderStage>;
RADRAY_FLAG_ENUM(ShaderStage, ShaderStages);

enum class ShaderBlobCategory {
    DXIL,
    SPIRV,
    MSL
};

enum class ShaderResourceType {
    CBuffer,
    Texture,
    Buffer,
    RWTexture,
    RWBuffer,
    Sampler,
    PushConstant,
    RayTracing
};

enum class AddressMode {
    ClampToEdge,
    Repeat,
    Mirror
};

enum class FilterMode {
    Nearest,
    Linear
};

enum class CompareFunction {
    Never,
    Less,
    Equal,
    LessEqual,
    Greater,
    NotEqual,
    GreaterEqual,
    Always
};

enum class VertexStepMode {
    Vertex,
    Instance
};

enum class VertexFormat {
    UNKNOWN,

    UINT8X2,
    UINT8X4,
    SINT8X2,
    SINT8X4,
    UNORM8X2,
    UNORM8X4,
    SNORM8X2,
    SNORM8X4,
    UINT16x2,
    UINT16x4,
    SINT16X2,
    SINT16X4,
    UNORM16X2,
    UNORM16X4,
    SNORM16X2,
    SNORM16X4,
    FLOAT16X2,
    FLOAT16X4,
    UINT32,
    UINT32X2,
    UINT32X3,
    UINT32X4,
    SINT32,
    SINT32X2,
    SINT32X3,
    SINT32X4,
    FLOAT32,
    FLOAT32X2,
    FLOAT32X3,
    FLOAT32X4,
};

enum class VertexSemantic {
    Position,
    Normal,
    Texcoord,
    Tangent,
    Color,
    PSize,
    BiNormal,
    BlendIndices,
    BlendWeight,
    PositionT
};

enum class PrimitiveTopology {
    PointList,
    LineList,
    LineStrip,
    TriangleList,
    TriangleStrip
};

enum class IndexFormat {
    UINT16,
    UINT32
};

enum class FrontFace {
    CCW,
    CW
};

enum class CullMode {
    Front,
    Back,
    None
};

enum class PolygonMode {
    Fill,
    Line,
    Point
};

enum class StencilOperation {
    Keep,
    Zero,
    Replace,
    Invert,
    IncrementClamp,
    DecrementClamp,
    IncrementWrap,
    DecrementWrap
};

enum class BlendFactor {
    Zero,
    One,
    Src,
    OneMinusSrc,
    SrcAlpha,
    OneMinusSrcAlpha,
    Dst,
    OneMinusDst,
    DstAlpha,
    OneMinusDstAlpha,
    SrcAlphaSaturated,
    Constant,
    OneMinusConstant,
    Src1,
    OneMinusSrc1,
    Src1Alpha,
    OneMinusSrc1Alpha
};

enum class BlendOperation {
    Add,
    Subtract,
    ReverseSubtract,
    Min,
    Max
};

enum class ColorWrite : uint32_t {
    Red = 0x1,
    Green = 0x2,
    Blue = 0x4,
    Alpha = 0x8,
    Color = Red | Green | Blue,
    All = Red | Green | Blue | Alpha
};
using ColorWrites = std::underlying_type_t<ColorWrite>;
RADRAY_FLAG_ENUM(ColorWrite, ColorWrites);

bool IsDepthStencilFormat(TextureFormat format) noexcept;

class RootSignature;
class Shader;

struct SamplerDescriptor {
    AddressMode AddressS;
    AddressMode AddressT;
    AddressMode AddressR;
    FilterMode MigFilter;
    FilterMode MagFilter;
    FilterMode MipmapFilter;
    float LodMin;
    float LodMax;
    CompareFunction Compare;
    uint32_t AnisotropyClamp;
};

class DxilReflection {
public:
    class Variable {
    public:
        radray::string Name;
        uint32_t Start;
        uint32_t Size;
    };

    class CBuffer {
    public:
        radray::string Name;
        radray::vector<Variable> Vars;
        uint32_t Size;
    };

    class BindResource {
    public:
        radray::string Name;
        ShaderResourceType Type;
        TextureDimension Dim;
        uint32_t Space;
        uint32_t BindPoint;
        uint32_t BindCount;
    };

    class VertexInput {
    public:
        VertexSemantic Semantic;
        uint32_t SemanticIndex;
        VertexFormat Format;
    };

public:
    radray::vector<CBuffer> CBuffers;
    radray::vector<BindResource> Binds;
    radray::vector<VertexInput> VertexInputs;
    radray::vector<radray::string> StaticSamplers;
    std::array<uint32_t, 3> GroupSize;
};

class SpirvReflection {};

class MslReflection {};

using ShaderReflection = std::variant<DxilReflection, SpirvReflection, MslReflection>;

struct VertexElement {
    uint64_t Offset;
    VertexSemantic Semantic;
    uint32_t SemanticIndex;
    VertexFormat Format;
    uint32_t Location;
};

class VertexBufferLayout {
public:
    uint64_t ArrayStride;
    VertexStepMode StepMode;
    std::vector<VertexElement> Elements;
};

struct PrimitiveState {
    PrimitiveTopology Topology;
    IndexFormat StripIndexFormat;
    FrontFace FaceClockwise;
    CullMode Cull;
    PolygonMode Poly;
    bool UnclippedDepth;
    bool Conservative;
};

struct StencilFaceState {
    CompareFunction Compare;
    StencilOperation FailOp;
    StencilOperation DepthFailOp;
    StencilOperation PassOp;
};

struct StencilState {
    StencilFaceState Front;
    StencilFaceState Back;
    uint32_t ReadMask;
    uint32_t WriteMask;
};

struct DepthBiasState {
    int32_t Constant;
    float SlopScale;
    float Clamp;
};

struct DepthStencilState {
    TextureFormat Format;
    CompareFunction DepthCompare;
    StencilState Stencil;
    DepthBiasState DepthBias;
    bool DepthWriteEnable;
    bool StencilEnable;
};

struct MultiSampleState {
    uint32_t Count;
    uint64_t Mask;
    bool AlphaTocoverageEnable;
};

struct BlendComponent {
    BlendFactor Src;
    BlendFactor Dst;
    BlendOperation Op;
};

struct BlendState {
    BlendComponent Color;
    BlendComponent Alpha;
};

struct ColorTargetState {
    TextureFormat Format;
    BlendState Blend;
    ColorWrites WriteMask;
    bool BlendEnable;
};

class GraphicsPipelineStateDescriptor {
public:
    std::string Name;
    RootSignature* RootSig;
    Shader* VS;
    Shader* PS;
    std::vector<VertexBufferLayout> VertexBuffers;
    PrimitiveState Primitive;
    DepthStencilState DepthStencil;
    MultiSampleState MultiSample;
    std::vector<ColorTargetState> ColorTargets;
    bool DepthStencilEnable;
};

class RenderBase : public Noncopyable {
public:
    virtual ~RenderBase() noexcept = default;

    virtual bool IsValid() const noexcept = 0;
    virtual void Destroy() noexcept = 0;
};

std::string_view format_as(Backend v) noexcept;
std::string_view format_as(TextureFormat v) noexcept;
std::string_view format_as(QueueType v) noexcept;
std::string_view format_as(ShaderBlobCategory v) noexcept;
std::string_view format_as(ShaderResourceType v) noexcept;
std::string_view format_as(VertexSemantic v) noexcept;
std::string_view format_as(VertexFormat v) noexcept;
std::string_view format_as(PolygonMode v) noexcept;

}  // namespace radray::render
