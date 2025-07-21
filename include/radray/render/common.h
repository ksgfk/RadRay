#pragma once

#include <variant>

#include <radray/types.h>
#include <radray/nullable.h>
#include <radray/enum_flags.h>
#include <radray/utility.h>
#include <radray/image_data.h>

namespace radray::render {

enum class Backend {
    D3D12,
    Vulkan,
    Metal,

    MAX_COUNT
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
    Dim3D
};

enum class TextureViewDimension {
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

    MAX_COUNT
};

enum class ShaderStage : uint32_t {
    UNKNOWN = 0x0,
    Vertex = 0x1,
    Pixel = Vertex << 1,
    Compute = Pixel << 1,

    Graphics = Vertex | Pixel
};

enum class ShaderBlobCategory {
    DXIL,
    SPIRV,
    MSL
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

enum class [[deprecated]] ResourceType {
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

enum class [[deprecated]] ResourceUsage : uint32_t {
    UNKNOWN = 0x0,

    Sampler = 0x1,

    Texture = Sampler << 1,
    RenderTarget = Texture << 1,
    DepthStencil = RenderTarget << 1,
    Cube = DepthStencil << 1,
    TextureRW = Cube << 1,

    Buffer = TextureRW << 1,
    CBuffer = Buffer << 1,
    PushConstant = CBuffer << 1,
    VertexBuffer = PushConstant << 1,
    IndexBuffer = VertexBuffer << 1,
    BufferRW = IndexBuffer << 1,

    RayTracing = BufferRW << 1
};

enum class TextureUse : uint32_t {
    UNKNOWN = 0x0,
    Uninitialized = 0x1,
    Present = Uninitialized << 1,
    CopySource = Present << 1,
    CopyDestination = CopySource << 1,
    Resource = CopyDestination << 1,
    RenderTarget = Resource << 1,
    DepthStencilRead = RenderTarget << 1,
    DepthStencilWrite = DepthStencilRead << 1,
    StorageRead = DepthStencilWrite << 1,
    StorageWrite = StorageRead << 1,
    StorageRW = StorageWrite << 1
};

enum class [[deprecated]] ResourceState : uint32_t {
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

enum class [[deprecated]] ResourceMemoryUsage {
    Default,
    Upload,
    Readback
};

enum class ResourceHint : uint32_t {
    None = 0x0,
    Dedicated = 0x1
};

enum class LoadAction {
    DontCare,
    Load,
    Clear
};

enum class StoreAction {
    Store,
    Discard
};

enum class RenderObjectTag : uint32_t {
    UNKNOWN = 0x0,
    Device = 0x1,
    CmdQueue = Device << 1,
    CmdBuffer = CmdQueue << 1,
    CmdEncoder = CmdBuffer << 1,
    Fence = CmdEncoder << 1,
    Semaphore = Fence << 1,
    Shader = Semaphore << 1,
    RootSignature = Shader << 1,
    PipelineState = RootSignature << 1,
    GraphicsPipelineState = PipelineState | (PipelineState << 1),
    SwapChain = PipelineState << 2,
    Resource = SwapChain << 1,
    Buffer = Resource | (Resource << 1),
    Texture = Resource | (Resource << 2),
    ResourceView = Resource << 3,
    BufferView = ResourceView | (ResourceView << 1),
    TextureView = ResourceView | (ResourceView << 2),
    DescriptorSet = ResourceView << 3,
    Sampler = DescriptorSet << 1
};

class Device;
class CommandQueue;
class CommandBuffer;
class CommandEncoder;
class Fence;
class Semaphore;
class Shader;
class RootSignature;
class PipelineState;
class GraphicsPipelineState;
class SwapChain;
class Resource;
class Buffer;
class Texture;
class ResourceView;
class BufferView;
class TextureView;
class DescriptorSet;
class Sampler;

bool IsDepthStencilFormat(TextureFormat format) noexcept;
uint32_t GetVertexFormatSize(VertexFormat format) noexcept;
TextureFormat ImageToTextureFormat(radray::ImageFormat fmt) noexcept;

struct ColorClearValue {
    float R, G, B, A;
};

struct DepthStencilClearValue {
    float Depth;
    uint8_t Stencil;
};

using ClearValue = std::variant<ColorClearValue, DepthStencilClearValue>;

std::string_view format_as(Backend v) noexcept;
std::string_view format_as(TextureFormat v) noexcept;
std::string_view format_as(QueueType v) noexcept;
std::string_view format_as(ShaderBlobCategory v) noexcept;
std::string_view format_as(VertexFormat v) noexcept;
std::string_view format_as(PolygonMode v) noexcept;
std::string_view format_as(ResourceType v) noexcept;
std::string_view format_as(ResourceMemoryUsage v) noexcept;
std::string_view format_as(TextureViewDimension v) noexcept;

}  // namespace radray::render

namespace radray {

template <>
struct is_flags<render::ShaderStage> : public std::true_type {};
template <>
struct is_flags<render::ColorWrite> : public std::true_type {};
template <>
struct is_flags<render::ResourceState> : public std::true_type {};
template <>
struct is_flags<render::ResourceHint> : public std::true_type {};
template <>
struct is_flags<render::ResourceUsage> : public std::true_type {};
template <>
struct is_flags<render::RenderObjectTag> : public std::true_type {};
template <>
struct is_flags<render::TextureUse> : public std::true_type {};

}  // namespace radray

namespace radray::render {

using ShaderStages = EnumFlags<render::ShaderStage>;
using ColorWrites = EnumFlags<render::ColorWrite>;
using ResourceStates = EnumFlags<render::ResourceState>;
using ResourceHints = EnumFlags<render::ResourceHint>;
using ResourceUsages = EnumFlags<render::ResourceUsage>;
using RenderObjectTags = EnumFlags<render::RenderObjectTag>;
using TextureUses = EnumFlags<render::TextureUse>;

class Device1;
class CommandQueue1;
class CommandBuffer1;
class Fence1;
class Semaphore1;
class SwapChain1;

class RenderBase : public Noncopyable {
public:
    virtual ~RenderBase() noexcept = default;

    virtual RenderObjectTags GetTag() const noexcept = 0;
    virtual bool IsValid() const noexcept = 0;
    virtual void Destroy() noexcept = 0;
};

class D3D12BackendInitDescriptor {
public:
};

class MetalBackendInitDescriptor {
public:
};

class VulkanBackendInitDescriptor {
public:
    bool IsEnableDebugLayer;
    bool IsEnableGpuBasedValid;
};

using BackendInitDescriptor = std::variant<D3D12BackendInitDescriptor, MetalBackendInitDescriptor, VulkanBackendInitDescriptor>;

class D3D12DeviceDescriptor {
public:
    std::optional<uint32_t> AdapterIndex;
    bool IsEnableDebugLayer;
    bool IsEnableGpuBasedValid;
};

class MetalDeviceDescriptor {
public:
    std::optional<uint32_t> DeviceIndex;
};

struct VulkanCommandQueueDescriptor {
    QueueType Type;
    uint32_t Count;
};

class VulkanDeviceDescriptor {
public:
    std::optional<uint32_t> PhysicalDeviceIndex;
    std::span<VulkanCommandQueueDescriptor> Queues;
};

using DeviceDescriptor = std::variant<D3D12DeviceDescriptor, MetalDeviceDescriptor, VulkanDeviceDescriptor>;

class SwapChainDescriptor {
public:
    CommandQueue1* PresentQueue;
    const void* NativeHandler;
    uint32_t Width;
    uint32_t Height;
    uint32_t BackBufferCount;
    TextureFormat Format;
};

class Device1 : public enable_shared_from_this<Device1>, public RenderBase {
public:
    virtual ~Device1() noexcept = default;

    RenderObjectTags GetTag() const noexcept final { return RenderObjectTag::Device; }

    virtual Backend GetBackend() noexcept = 0;

    virtual Nullable<CommandQueue1> GetCommandQueue(QueueType type, uint32_t slot = 0) noexcept = 0;

    virtual Nullable<shared_ptr<CommandBuffer1>> CreateCommandBuffer(CommandQueue1* queue) noexcept = 0;

    virtual Nullable<shared_ptr<Fence1>> CreateFence() noexcept = 0;

    virtual Nullable<shared_ptr<Semaphore1>> CreateGpuSemaphore() noexcept = 0;

    virtual Nullable<shared_ptr<SwapChain1>> CreateSwapChain(const SwapChainDescriptor& desc) noexcept = 0;
};

class CommandQueue1 : public RenderBase {
public:
    virtual ~CommandQueue1() noexcept = default;

    RenderObjectTags GetTag() const noexcept final { return RenderObjectTag::CmdQueue; }
};

class CommandBuffer1 : public RenderBase {
public:
    virtual ~CommandBuffer1() noexcept = default;

    RenderObjectTags GetTag() const noexcept final { return RenderObjectTag::CmdBuffer; }
};

class Fence1 : public RenderBase {
public:
    virtual ~Fence1() noexcept = default;

    RenderObjectTags GetTag() const noexcept final { return RenderObjectTag::Fence; }
};

class Semaphore1 : public RenderBase {
public:
    virtual ~Semaphore1() noexcept = default;

    RenderObjectTags GetTag() const noexcept final { return RenderObjectTag::Semaphore; }
};

class SwapChain1 : public RenderBase {
public:
    virtual ~SwapChain1() noexcept = default;

    RenderObjectTags GetTag() const noexcept final { return RenderObjectTag::SwapChain; }
};

}  // namespace radray::render
