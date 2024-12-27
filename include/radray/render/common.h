#pragma once

#include <radray/types.h>
#include <radray/utility.h>
#include <radray/enum_flags.h>

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
using ShaderStages = EnumFlags<ShaderStage>;

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
    UINT16X2,
    UINT16X4,
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
using ColorWrites = EnumFlags<ColorWrite>;

enum class ResourceType {
    UNKNOWN,

    Sampler,

    Texture,
    RenderTarget,
    DepthStencil,
    TextureRW,

    Buffer,
    CBuffer,
    PushConstant,
    BufferRW,

    RayTracing
};

enum class ResourceState : uint32_t {
    Common = 0x0,

    VertexAndConstantBuffer = 0x1,
    IndexBuffer = 0x2,
    ShaderResource = 0x4,
    IndirectArgument = 0x8,
    CopySource = 0x10,
    GenericRead = VertexAndConstantBuffer | IndexBuffer | ShaderResource | IndirectArgument | CopySource,

    RenderTarget = 0x20,
    StreamOut = 0x40,
    CopyDestination = 0x80,

    UnorderedAccess = 0x100,

    DepthWrite = 0x200,
    DepthRead = 0x400,
    AccelerationStructure = 0x800,
    Present = 0x1000
};
using ResourceStates = EnumFlags<ResourceState>;

enum class ResourceUsage {
    Default,
    Upload,
    Readback
};

enum class ResourceMemoryTip : uint32_t {
    None = 0x0,
    Dedicated = 0x1,
    PersistentMap = 0x2
};
using ResourceMemoryTips = EnumFlags<ResourceMemoryTip>;

class Device;
class CommandQueue;
class CommandPool;
class CommandBuffer;
class Fence;
class Shader;
class RootSignature;
class GraphicsPipelineState;
class SwapChain;
class Buffer;
class Texture;

bool IsDepthStencilFormat(TextureFormat format) noexcept;
uint32_t GetVertexFormatSize(VertexFormat format) noexcept;

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
std::string_view format_as(ResourceType v) noexcept;
std::string_view format_as(ResourceUsage v) noexcept;

}  // namespace radray::render

namespace radray::detail {

template <>
struct is_flags<render::ShaderStage> : public std::true_type {};

template <>
struct is_flags<render::ColorWrite> : public std::true_type {};

template <>
struct is_flags<render::ResourceState> : public std::true_type {};

template <>
struct is_flags<render::ResourceMemoryTip> : public std::true_type {};

}  // namespace radray::detail
