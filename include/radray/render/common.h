#pragma once

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
    Dim1D,
    Dim2D,
    Dim3D,
    Cube
};

enum class QueueType : uint32_t {
    Direct,
    Compute,
    Copy,
};

enum class ShaderStage : uint32_t {
    Vertex = 0x1,
    Pixel = 0x2,
    Compute = 0x4
};
using ShaderStages = std::underlying_type_t<ShaderStage>;
constexpr ShaderStages operator|(ShaderStages l, ShaderStage r) noexcept {
    return static_cast<ShaderStages>(static_cast<std::underlying_type_t<ShaderStage>>(l) | static_cast<std::underlying_type_t<ShaderStage>>(r));
}
constexpr ShaderStages& operator|=(ShaderStages& l, ShaderStage r) noexcept {
    l = l | r;
    return l;
}
constexpr ShaderStage operator&(ShaderStages l, ShaderStage r) noexcept {
    return static_cast<ShaderStage>(static_cast<std::underlying_type_t<ShaderStage>>(l) & static_cast<std::underlying_type_t<ShaderStage>>(r));
}
constexpr bool HasFlag(ShaderStages that, ShaderStage l) noexcept {
    return (that & l) == l;
}

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
    PushConstant
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
    // TODO
};

enum class VertexSemantic {
    // TODO
};

enum class PrimitiveTopology {
    // TODO
};

enum class IndexFormat {
    // TODO
};

enum class FrontFace {
    // TODO
};

enum class CullMode {
    // TODO
};

enum class PolygonMode {
    // TODO
};

enum class StencilOperation {
    // TODO
};

enum class BlendFactor {
    // TODO
};

enum class BlendOperation {
    // TODO
};

enum class ColorWrite : uint32_t {
    // TODO
};
using ColorWrites = std::underlying_type_t<ColorWrite>;

bool IsDepthStencilFormat(TextureFormat format) noexcept;

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
    bool Conservative;
};

struct StencilFaceState {
    CompareFunction Compare;
    StencilOperation FailOp;
    StencilOperation DepthFailOp;
    StencilOperation ColorOp;
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
    bool BlendEnable;
};

struct ColorTargetState {
    TextureFormat Format;
    BlendState Blend;
    ColorWrites WriteMask;
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

}  // namespace radray::render
