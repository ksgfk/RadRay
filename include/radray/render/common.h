#pragma once

#include <variant>
#include <optional>
#include <span>

#include <radray/types.h>
#include <radray/nullable.h>
#include <radray/enum_flags.h>

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

enum struct BufferUse : uint32_t {
    UNKNOWN = 0x0,
    MapRead = 0x1,
    MapWrite = MapRead << 1,
    CopySource = MapWrite << 1,
    CopyDestination = CopySource << 1,
    Index = CopyDestination << 1,
    Vertex = Index << 1,
    CBuffer = Vertex << 1,
    StorageRead = CBuffer << 1,
    StorageRW = StorageRead << 1,
    Indirect = StorageRW << 1
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

enum class FenceState {
    Complete,
    Incomplete,
    NotSubmitted
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

}  // namespace radray::render

namespace radray {

template <>
struct is_flags<render::ShaderStage> : public std::true_type {};
template <>
struct is_flags<render::ColorWrite> : public std::true_type {};
template <>
struct is_flags<render::ResourceHint> : public std::true_type {};
template <>
struct is_flags<render::RenderObjectTag> : public std::true_type {};
template <>
struct is_flags<render::BufferUse> : public std::true_type {};
template <>
struct is_flags<render::TextureUse> : public std::true_type {};

namespace render {

using ShaderStages = EnumFlags<render::ShaderStage>;
using ColorWrites = EnumFlags<render::ColorWrite>;
using ResourceHints = EnumFlags<render::ResourceHint>;
using RenderObjectTags = EnumFlags<render::RenderObjectTag>;
using BufferUses = EnumFlags<render::BufferUse>;
using TextureUses = EnumFlags<render::TextureUse>;

}  // namespace render

}  // namespace radray

namespace radray::render {

struct ColorClearValue {
    float R, G, B, A;
};

struct DepthStencilClearValue {
    float Depth;
    uint8_t Stencil;
};

using ClearValue = std::variant<ColorClearValue, DepthStencilClearValue>;

class Device;
class CommandQueue;
class CommandBuffer;
class Fence;
class Semaphore;
class SwapChain;
class Buffer;
class Texture;

class RenderBase {
public:
    RenderBase() noexcept = default;
    virtual ~RenderBase() noexcept = default;
    RenderBase(const RenderBase&) = delete;
    RenderBase(RenderBase&&) = default;
    RenderBase& operator=(const RenderBase&) = delete;
    RenderBase& operator=(RenderBase&&) = default;

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
    CommandQueue* PresentQueue;
    const void* NativeHandler;
    uint32_t Width;
    uint32_t Height;
    uint32_t BackBufferCount;
    TextureFormat Format;
    bool EnableSync;
};

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
    bool HasCompare;

    friend bool operator==(const SamplerDescriptor& lhs, const SamplerDescriptor& rhs) noexcept;
    friend bool operator!=(const SamplerDescriptor& lhs, const SamplerDescriptor& rhs) noexcept;
};

struct CommandQueueSubmitDescriptor {
    std::span<CommandBuffer*> CmdBuffers;
    Nullable<Fence> SignalFence;
    std::span<Semaphore*> WaitSemaphores;
    std::span<Semaphore*> SignalSemaphores;
};

struct CommandQueuePresentDescriptor {
    SwapChain* Target;
    std::span<Semaphore*> WaitSemaphores;
};

struct BarrierBufferDescriptor {
    Buffer* Target;
    BufferUses Before;
    BufferUses After;
    Nullable<CommandQueue> OtherQueue;
    bool IsFromOrToOtherQueue; // true: from, false: to
};

struct BarrierTextureDescriptor {
    Texture* Target;
    TextureUses Before;
    TextureUses After;
    Nullable<CommandQueue> OtherQueue;
    bool IsFromOrToOtherQueue;
    bool IsSubresourceBarrier;
    uint32_t BaseArrayLayer;
    uint32_t ArrayLayerCount;
    uint32_t BaseMipLevel;
    uint32_t MipLevelCount;
};

struct SwapChainAcquireNextDescriptor {
    Nullable<Semaphore> SignalSemaphore;
    Nullable<Fence> WaitFence;
};

class Device : public enable_shared_from_this<Device>, public RenderBase {
public:
    virtual ~Device() noexcept = default;

    RenderObjectTags GetTag() const noexcept final { return RenderObjectTag::Device; }

    virtual Backend GetBackend() noexcept = 0;

    virtual Nullable<CommandQueue> GetCommandQueue(QueueType type, uint32_t slot = 0) noexcept = 0;

    virtual Nullable<shared_ptr<CommandBuffer>> CreateCommandBuffer(CommandQueue* queue) noexcept = 0;

    virtual Nullable<shared_ptr<Fence>> CreateFence() noexcept = 0;

    virtual void WaitFences(std::span<Fence*> fences) noexcept = 0;

    virtual Nullable<shared_ptr<Semaphore>> CreateGpuSemaphore() noexcept = 0;

    virtual Nullable<shared_ptr<SwapChain>> CreateSwapChain(const SwapChainDescriptor& desc) noexcept = 0;
};

class CommandQueue : public RenderBase {
public:
    virtual ~CommandQueue() noexcept = default;

    RenderObjectTags GetTag() const noexcept final { return RenderObjectTag::CmdQueue; }

    virtual void Submit(const CommandQueueSubmitDescriptor& desc) noexcept = 0;

    virtual void Present(const CommandQueuePresentDescriptor& desc) noexcept = 0;

    virtual void WaitIdle() noexcept = 0;
};

class CommandBuffer : public RenderBase {
public:
    virtual ~CommandBuffer() noexcept = default;

    RenderObjectTags GetTag() const noexcept final { return RenderObjectTag::CmdBuffer; }

    virtual void Begin() noexcept = 0;

    virtual void End() noexcept = 0;

    virtual void ResourceBarrier(std::span<BarrierBufferDescriptor> buffers, std::span<BarrierTextureDescriptor> textures) noexcept = 0;
};

class Fence : public RenderBase {
public:
    virtual ~Fence() noexcept = default;

    RenderObjectTags GetTag() const noexcept final { return RenderObjectTag::Fence; }

    virtual FenceState GetState() const noexcept = 0;
};

class Semaphore : public RenderBase {
public:
    virtual ~Semaphore() noexcept = default;

    RenderObjectTags GetTag() const noexcept final { return RenderObjectTag::Semaphore; }
};

class SwapChain : public RenderBase {
public:
    virtual ~SwapChain() noexcept = default;

    RenderObjectTags GetTag() const noexcept final { return RenderObjectTag::SwapChain; }

    virtual Nullable<Texture> AcquireNextTexture(const SwapChainAcquireNextDescriptor& desc) noexcept = 0;

    virtual Nullable<Texture> GetCurrentBackBuffer() noexcept = 0;

    virtual uint32_t GetCurrentBackBufferIndex() const noexcept = 0;
};

class Resource : public RenderBase {
public:
    virtual ~Resource() noexcept = default;
};

class Buffer : public Resource {
public:
    virtual ~Buffer() noexcept = default;

    RenderObjectTags GetTag() const noexcept final { return RenderObjectTag::Buffer; }
};

class Texture : public Resource {
public:
    virtual ~Texture() noexcept = default;

    RenderObjectTags GetTag() const noexcept final { return RenderObjectTag::Texture; }
};

bool GlobalInitGraphics(std::span<BackendInitDescriptor> descs);

void GlobalTerminateGraphics();

Nullable<shared_ptr<Device>> CreateDevice(const DeviceDescriptor& desc);

bool IsDepthStencilFormat(TextureFormat format) noexcept;

uint32_t GetVertexFormatSize(VertexFormat format) noexcept;

std::string_view format_as(Backend v) noexcept;
std::string_view format_as(TextureFormat v) noexcept;
std::string_view format_as(QueueType v) noexcept;
std::string_view format_as(ShaderBlobCategory v) noexcept;
std::string_view format_as(VertexFormat v) noexcept;
std::string_view format_as(PolygonMode v) noexcept;
std::string_view format_as(TextureViewDimension v) noexcept;

}  // namespace radray::render
