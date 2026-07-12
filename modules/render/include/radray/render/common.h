#pragma once

#include <array>
#include <limits>
#include <variant>
#include <optional>
#include <span>

#include <radray/types.h>
#include <radray/nullable.h>
#include <radray/enum_flags.h>
#include <radray/basic_math.h>
#include <radray/guid.h>

#include <radray/render/shader/hlsl.h>
#include <radray/render/shader/spirv.h>

namespace radray::render {

// PSO 缓存 POD key 的容量上限. 超限时 BuildKey 返回失败.
inline constexpr uint32_t kMaxColorTargets = 8;
inline constexpr uint32_t kMaxVertexBufferLayouts = 16;
inline constexpr uint32_t kMaxVertexElementsPerLayout = 16;
inline constexpr uint32_t kMaxSemanticLength = 32;

enum class RenderBackend : int32_t {
    D3D12,
    Vulkan,
    Metal,

    MAX_COUNT
};

enum class TextureFormat : int32_t {
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

    D16_UNORM,
    D32_FLOAT,
    D24_UNORM_S8_UINT,
    D32_FLOAT_S8_UINT,
};

enum class TextureDimension : int32_t {
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

    RayGen = Compute << 1,
    Miss = RayGen << 1,
    ClosestHit = Miss << 1,
    AnyHit = ClosestHit << 1,
    Intersection = AnyHit << 1,
    Callable = Intersection << 1,

    Graphics = Vertex | Pixel,
    RayTracing = RayGen | Miss | ClosestHit | AnyHit | Intersection | Callable
};

enum class ShaderBlobCategory : int32_t {
    DXIL,
    SPIRV,
    MSL,
    METALLIB
};

enum class AddressMode : int32_t {
    ClampToEdge,
    Repeat,
    Mirror
};

enum class FilterMode : int32_t {
    Nearest,
    Linear
};

enum class CompareFunction : int32_t {
    Never,
    Less,
    Equal,
    LessEqual,
    Greater,
    NotEqual,
    GreaterEqual,
    Always
};

enum class VertexStepMode : int32_t {
    Vertex,
    Instance
};

enum class VertexFormat : int32_t {
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

enum class PrimitiveTopology : int32_t {
    PointList,
    LineList,
    LineStrip,
    TriangleList,
    TriangleStrip
};

enum class IndexFormat : int32_t {
    UINT16,
    UINT32
};

enum class FrontFace : int32_t {
    CCW,
    CW
};

enum class CullMode : int32_t {
    Front,
    Back,
    None
};

enum class PolygonMode : int32_t {
    Fill,
    Line,
    Point
};

enum class StencilOperation : int32_t {
    Keep,
    Zero,
    Replace,
    Invert,
    IncrementClamp,
    DecrementClamp,
    IncrementWrap,
    DecrementWrap
};

enum class BlendFactor : int32_t {
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

enum class BlendOperation : int32_t {
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
    Common = 0x1,
    MapRead = Common << 1,
    MapWrite = MapRead << 1,
    CopySource = MapWrite << 1,
    CopyDestination = CopySource << 1,
    Index = CopyDestination << 1,
    Vertex = Index << 1,
    CBuffer = Vertex << 1,
    Resource = CBuffer << 1,
    UnorderedAccess = Resource << 1,
    Indirect = UnorderedAccess << 1,
    AccelerationStructure = Indirect << 1,
    Scratch = AccelerationStructure << 1,
    ShaderTable = Scratch << 1
};

enum class TextureUse : uint32_t {
    UNKNOWN = 0x0,
    CopySource = 0x1,
    CopyDestination = CopySource << 1,
    Resource = CopyDestination << 1,
    RenderTarget = Resource << 1,
    DepthStencilRead = RenderTarget << 1,
    DepthStencilWrite = DepthStencilRead << 1,
    UnorderedAccess = DepthStencilWrite << 1,
};

enum class BufferState : uint32_t {
    UNKNOWN = 0x0,
    Undefined = 0x1,
    Common = Undefined << 1,
    CopySource = Common << 1,
    CopyDestination = CopySource << 1,
    Vertex = CopyDestination << 1,
    Index = Vertex << 1,
    CBuffer = Index << 1,
    ShaderRead = CBuffer << 1,
    UnorderedAccess = ShaderRead << 1,
    Indirect = UnorderedAccess << 1,
    HostRead = Indirect << 1,
    HostWrite = HostRead << 1,
    AccelerationStructureBuildInput = HostWrite << 1,
    AccelerationStructureBuildScratch = AccelerationStructureBuildInput << 1,
    AccelerationStructureRead = AccelerationStructureBuildScratch << 1,
    ShaderTable = AccelerationStructureRead << 1
};

enum class TextureState : uint32_t {
    UNKNOWN = 0x0,
    Undefined = 0x1,
    Common = Undefined << 1,
    Present = Common << 1,
    CopySource = Present << 1,
    CopyDestination = CopySource << 1,
    ShaderRead = CopyDestination << 1,
    RenderTarget = ShaderRead << 1,
    DepthRead = RenderTarget << 1,
    DepthWrite = DepthRead << 1,
    UnorderedAccess = DepthWrite << 1,
    ResolveSource = UnorderedAccess << 1,
    ResolveDestination = ResolveSource << 1
};

enum class BufferViewUsage : uint32_t {
    CBuffer,
    ReadOnlyStorage,
    ReadWriteStorage,
    TexelReadOnly,
    TexelReadWrite
};

enum class TextureViewUsage : uint32_t {
    UNKNOWN = 0x0,
    Resource = 0x1,
    RenderTarget = Resource << 1,
    DepthRead = RenderTarget << 1,
    DepthWrite = DepthRead << 1,
    UnorderedAccess = DepthWrite << 1,
};

enum class ResourceHint : uint32_t {
    None = 0x0,
    Dedicated = 0x1,
    External = Dedicated << 1,
    /// Keeps a mappable buffer mapped for its lifetime. BufferUse::MapRead or
    /// BufferUse::MapWrite must also be present.
    PersistentMap = External << 1,
};

enum class LoadAction : int32_t {
    DontCare,
    Load,
    Clear
};

enum class StoreAction : int32_t {
    Store,
    Discard
};

enum class MemoryType : int32_t {
    Device,
    Upload,
    ReadBack,
};

enum class ResourceBindType : int32_t {
    UNKNOWN,
    CBuffer,
    Buffer,
    TexelBuffer,
    Texture,
    Sampler,
    RWBuffer,
    RWTexelBuffer,
    RWTexture,
    AccelerationStructure
};

enum class PresentMode : int32_t {
    FIFO,
    Mailbox,
    Immediate
};

enum class BindlessSlotType : int32_t {
    Multiple,
    BufferOnly,
    Texture2DOnly,
    Texture3DOnly
};

enum class AccelerationStructureType : int32_t {
    BottomLevel,
    TopLevel
};

enum class AccelerationStructureBuildMode : int32_t {
    Build,
    Update
};

enum class AccelerationStructureBuildFlag : uint32_t {
    None = 0x0,
    PreferFastTrace = 0x1,
    PreferFastBuild = PreferFastTrace << 1,
    AllowUpdate = PreferFastBuild << 1,
    AllowCompaction = AllowUpdate << 1
};

enum class ShaderBindingTableEntryType : uint8_t {
    RayGen,
    Miss,
    HitGroup,
    Callable
};

enum class ShaderParameterKind : int32_t {
    UNKNOWN,
    Resource,
    Sampler,
    Constant,
    BindlessArray
};

enum class SwapChainStatus : int32_t {
    Error,
    Success,
    RetryLater,
    RequireRecreate
};

enum class QueryType : int32_t {
    Timestamp
};

enum class QueryPipelineStage : int32_t {
    Top,
    Bottom,
    Graphics,
    Compute,
    RayTracing
};

enum class PhysicalDeviceType : int32_t {
    Other,
    IntegratedGpu,
    DiscreteGpu,
    VirtualGpu,
    Cpu,
};

enum class RenderObjectTag : uint32_t {
    UNKNOWN = 0x0,
    Device = 0x1,
    CmdQueue = Device << 1,
    CmdBuffer = CmdQueue << 1,
    CmdEncoder = CmdBuffer << 1,
    GraphicsCmdEncoder = CmdEncoder | (CmdEncoder << 1),
    ComputeCmdEncoder = CmdEncoder | (CmdEncoder << 2),
    Fence = CmdEncoder << 3,
    Shader = Fence << 1,
    PipelineLayout = Shader << 1,
    DescriptorPool = PipelineLayout << 1,
    BindingGroup = DescriptorPool << 1,
    PipelineState = BindingGroup << 1,
    GraphicsPipelineState = PipelineState | (PipelineState << 1),
    ComputePipelineState = PipelineState | (PipelineState << 2),
    SwapChain = PipelineState << 3,
    Resource = SwapChain << 1,
    Buffer = Resource | (Resource << 1),
    Texture = Resource | (Resource << 2),
    ResourceView = Resource << 3,
    TextureView = ResourceView | (ResourceView << 1),
    AccelerationStructureView = ResourceView | (ResourceView << 2),
    Sampler = ResourceView << 3,
    BindlessArray = Sampler << 1,
    QueryPool = BindlessArray << 1,
    RayTracingCmdEncoder = QueryPool << 1,
    AccelerationStructure = RayTracingCmdEncoder << 1,
    RayTracingPipelineState = AccelerationStructure << 1,
    ShaderBindingTable = RayTracingPipelineState << 1,
    RenderPass = ShaderBindingTable << 1,
    Framebuffer = RenderPass << 1,
    VkInstance = Framebuffer << 1,
    DXGIFactory = VkInstance << 1
};

}  // namespace radray::render

namespace radray {

template <>
struct is_flags<render::ShaderStage> : public std::true_type {};
template <>
struct is_compound_enum_flags<render::ShaderStage> : public std::true_type {};

template <>
struct is_flags<render::ColorWrite> : public std::true_type {};
template <>
struct is_compound_enum_flags<render::ColorWrite> : public std::true_type {};

template <>
struct is_flags<render::ResourceHint> : public std::true_type {};

template <>
struct is_flags<render::RenderObjectTag> : public std::true_type {};
template <>
struct is_compound_enum_flags<render::RenderObjectTag> : public std::true_type {};

template <>
struct is_flags<render::BufferUse> : public std::true_type {};
template <>
struct is_flags<render::TextureUse> : public std::true_type {};
template <>
struct is_flags<render::TextureViewUsage> : public std::true_type {};
template <>
struct is_flags<render::BufferState> : public std::true_type {};
template <>
struct is_flags<render::TextureState> : public std::true_type {};
template <>
struct is_flags<render::AccelerationStructureBuildFlag> : public std::true_type {};

namespace render {

using ShaderStages = EnumFlags<render::ShaderStage>;
using ColorWrites = EnumFlags<render::ColorWrite>;
using ResourceHints = EnumFlags<render::ResourceHint>;
using RenderObjectTags = EnumFlags<render::RenderObjectTag>;
using BufferUses = EnumFlags<render::BufferUse>;
using TextureUses = EnumFlags<render::TextureUse>;
using TextureViewUsages = EnumFlags<render::TextureViewUsage>;
using BufferStates = EnumFlags<render::BufferState>;
using TextureStates = EnumFlags<render::TextureState>;
using AccelerationStructureBuildFlags = EnumFlags<render::AccelerationStructureBuildFlag>;

}  // namespace render

}  // namespace radray

namespace radray::render {

class Device;
class CommandQueue;
class CommandBuffer;
class CommandEncoder;
class GraphicsCommandEncoder;
class ComputeCommandEncoder;
class RayTracingCommandEncoder;
class Fence;
class SwapChain;
class SwapChainSyncObject;
class SwapChainFrame;
class Resource;
class ResourceView;
class Buffer;
class Texture;
class TextureView;
class AccelerationStructureView;
class Shader;
class PipelineLayout;
class DescriptorPool;
class BindingGroup;
class RenderPass;
class Framebuffer;
class PipelineState;
class GraphicsPipelineState;
class ComputePipelineState;
class RayTracingPipelineState;
class ShaderBindingTable;
class AccelerationStructure;
class Sampler;
class BindlessArray;
class QueryPool;
class InstanceVulkan;
class DXGIFactory;

struct ColorClearValue {
    std::array<float, 4> Value{};
};

struct DepthStencilClearValue {
    float Depth;
    uint8_t Stencil;
};

using ClearValue = std::variant<ColorClearValue, DepthStencilClearValue>;

class RenderBase {
public:
    RenderBase() noexcept = default;
    virtual ~RenderBase() noexcept = default;
    RenderBase(const RenderBase&) = delete;
    RenderBase(RenderBase&&) = delete;
    RenderBase& operator=(const RenderBase&) = delete;
    RenderBase& operator=(RenderBase&&) = delete;

    virtual RenderObjectTags GetTag() const noexcept = 0;

    virtual bool IsValid() const noexcept = 0;

    virtual void Destroy() noexcept = 0;
};

class IDebugName {
public:
    virtual ~IDebugName() noexcept = default;

    virtual void SetDebugName(std::string_view name) noexcept = 0;
};

using RenderLogCallback = void (*)(LogLevel level, std::string_view message, void* userData);

struct VulkanInstanceDescriptor {
    std::string_view AppName{};
    uint32_t AppVersion{0};
    std::string_view EngineName{};
    uint32_t EngineVersion{0};
    bool IsEnableDebugLayer{false};
    bool IsEnableGpuBasedValid{false};
    RenderLogCallback LogCallback{nullptr};
    void* LogUserData{nullptr};
};

struct DXGIFactoryDescriptor {
    bool IsEnableDebugLayer{false};
    bool IsEnableGpuBasedValid{false};
    RenderLogCallback LogCallback{nullptr};
    void* LogUserData{nullptr};
};

struct VulkanPhysicalDeviceInfo {
    uint32_t Index{0};
    string Name{};
    PhysicalDeviceType Type{PhysicalDeviceType::Other};
    uint32_t VendorId{0};
    uint32_t DeviceId{0};
    uint32_t ApiVersionMajor{0};
    uint32_t ApiVersionMinor{0};
    uint32_t ApiVersionPatch{0};
    uint64_t DeviceLocalMemoryBytes{0};
    bool IsUma{false};
};

struct DXGIAdapterInfo {
    uint32_t Index{0};
    string Name{};
    PhysicalDeviceType Type{PhysicalDeviceType::Other};
    uint32_t VendorId{0};
    uint32_t DeviceId{0};
    uint64_t DedicatedVideoMemoryBytes{0};
    uint64_t DedicatedSystemMemoryBytes{0};
    uint64_t SharedSystemMemoryBytes{0};
    bool IsSoftware{false};
    bool IsD3D12Supported{false};
};

struct D3D12DeviceDescriptor {
    DXGIFactory* Factory{nullptr};
    std::optional<uint32_t> AdapterIndex{};
};

struct MetalDeviceDescriptor {
    std::optional<uint32_t> DeviceIndex{};
};

struct VulkanCommandQueueDescriptor {
    QueueType Type{QueueType::Direct};
    uint32_t Count{0};
};

class VulkanDeviceDescriptor {
public:
    std::optional<uint32_t> PhysicalDeviceIndex{};
    std::span<const VulkanCommandQueueDescriptor> Queues{};
    bool EnableRayTracing{true};
};

using DeviceDescriptor = std::variant<D3D12DeviceDescriptor, MetalDeviceDescriptor, VulkanDeviceDescriptor>;

class SwapChainDescriptor {
public:
    CommandQueue* PresentQueue{nullptr};
    const void* NativeHandler{nullptr};
    uint32_t Width{0};
    uint32_t Height{0};
    uint32_t BackBufferCount{0};
    TextureFormat Format{TextureFormat::UNKNOWN};
    PresentMode PresentMode{PresentMode::FIFO};
};

struct QueryPoolDescriptor {
    QueryType Type{QueryType::Timestamp};
    uint32_t Count{0};
    string DebugName{};
};

struct QueryTimestampDescriptor {
    QueryPool* Pool{nullptr};
    QueryPipelineStage Stage{QueryPipelineStage::Bottom};
    uint32_t Index{0};
};

struct QueryResolveDescriptor {
    QueryPool* Pool{nullptr};
    uint32_t FirstIndex{0};
    uint32_t Count{0};
    Buffer* Destination{nullptr};
    uint64_t DestinationOffset{0};
};

struct TextureCopyDescriptor {
    Texture* Destination{nullptr};
    uint32_t DestinationMipLevel{0};
    uint32_t DestinationArrayLayer{0};
    uint32_t DestinationX{0};
    uint32_t DestinationY{0};
    uint32_t DestinationZ{0};
    Texture* Source{nullptr};
    uint32_t SourceMipLevel{0};
    uint32_t SourceArrayLayer{0};
    uint32_t SourceX{0};
    uint32_t SourceY{0};
    uint32_t SourceZ{0};
    uint32_t Width{0};
    uint32_t Height{0};
    uint32_t Depth{1};
    uint32_t ArrayLayerCount{1};
};

struct TextureResolveDescriptor {
    Texture* Destination{nullptr};
    uint32_t DestinationMipLevel{0};
    uint32_t DestinationArrayLayer{0};
    Texture* Source{nullptr};
    uint32_t SourceMipLevel{0};
    uint32_t SourceArrayLayer{0};
    uint32_t ArrayLayerCount{1};
};

struct TimestampQueryResult {
    uint64_t Tick{0};
    bool Valid{false};
};

struct TimestampQueryCalibration {
    uint64_t FrequencyHz{0};
    double TickPeriodNs{0.0};
};

struct SamplerDescriptor {
    AddressMode AddressS{};
    AddressMode AddressT{};
    AddressMode AddressR{};
    FilterMode MinFilter{};
    FilterMode MagFilter{};
    FilterMode MipmapFilter{};
    float LodMin{0.0f};
    float LodMax{0.0f};
    std::optional<CompareFunction> Compare{};
    uint32_t AnisotropyClamp{0};

    friend bool operator==(const SamplerDescriptor& lhs, const SamplerDescriptor& rhs) noexcept = default;
    friend bool operator!=(const SamplerDescriptor& lhs, const SamplerDescriptor& rhs) noexcept = default;
};

struct CommandQueueSubmitDescriptor {
    std::span<CommandBuffer*> CmdBuffers{};
    std::span<Fence*> SignalFences{};
    std::span<uint64_t> SignalValues{};
    std::span<Fence*> WaitFences{};
    std::span<uint64_t> WaitValues{};
    std::span<SwapChainSyncObject*> WaitToExecute{};
    std::span<SwapChainSyncObject*> ReadyToPresent{};
};

class SwapChainFrame {
public:
    SwapChainFrame() noexcept = default;
    ~SwapChainFrame() noexcept = default;

    SwapChainFrame(const SwapChainFrame&) = delete;
    SwapChainFrame& operator=(const SwapChainFrame&) = delete;

    SwapChainFrame(SwapChainFrame&& other) noexcept;
    SwapChainFrame& operator=(SwapChainFrame&& other) noexcept;

    Texture* GetBackBuffer() const noexcept;
    uint32_t GetBackBufferIndex() const noexcept;
    SwapChainSyncObject* GetWaitToDraw() const noexcept;
    SwapChainSyncObject* GetReadyToPresent() const noexcept;
    bool IsValid() const noexcept;

    friend constexpr void swap(SwapChainFrame& a, SwapChainFrame& b) noexcept {
        std::swap(a._owner, b._owner);
        std::swap(a._token, b._token);
        std::swap(a._backBuffer, b._backBuffer);
        std::swap(a._backBufferIndex, b._backBufferIndex);
        std::swap(a._waitToDraw, b._waitToDraw);
        std::swap(a._readyToPresent, b._readyToPresent);
    }

private:
    SwapChain* _owner{nullptr};
    uint64_t _token{0};
    Texture* _backBuffer{nullptr};
    uint32_t _backBufferIndex{std::numeric_limits<uint32_t>::max()};
    SwapChainSyncObject* _waitToDraw{nullptr};
    SwapChainSyncObject* _readyToPresent{nullptr};

    friend class SwapChain;
};

struct SwapChainAcquireResult {
    SwapChainStatus Status{SwapChainStatus::Error};
    int64_t NativeStatusCode{0};
    std::optional<SwapChainFrame> Frame{};
};

struct SwapChainPresentResult {
    int64_t NativeStatusCode{0};
    SwapChainStatus Status{SwapChainStatus::Error};
};

struct BarrierBufferDescriptor {
    Buffer* Target{nullptr};
    BufferStates Before{BufferState::UNKNOWN};
    BufferStates After{BufferState::UNKNOWN};
    Nullable<CommandQueue*> OtherQueue{nullptr};
    bool IsFromOrToOtherQueue{false};  // true: from, false: to
};

struct SubresourceRange {
    uint32_t BaseArrayLayer{0};
    uint32_t ArrayLayerCount{0};
    uint32_t BaseMipLevel{0};
    uint32_t MipLevelCount{0};

    static constexpr auto All = std::numeric_limits<uint32_t>::max();

    static constexpr SubresourceRange AllSub() noexcept {
        return SubresourceRange{0, SubresourceRange::All, 0, SubresourceRange::All};
    }
};

struct BarrierTextureDescriptor {
    Texture* Target{nullptr};
    TextureStates Before{TextureState::UNKNOWN};
    TextureStates After{TextureState::UNKNOWN};
    Nullable<CommandQueue*> OtherQueue{nullptr};
    bool IsFromOrToOtherQueue{false};
    bool IsSubresourceBarrier{false};
    SubresourceRange Range{};
};

struct BarrierAccelerationStructureDescriptor {
    AccelerationStructure* Target{nullptr};
    BufferStates Before{BufferState::UNKNOWN};
    BufferStates After{BufferState::UNKNOWN};
    Nullable<CommandQueue*> OtherQueue{nullptr};
    bool IsFromOrToOtherQueue{false};
};

using ResourceBarrierDescriptor = std::variant<BarrierBufferDescriptor, BarrierTextureDescriptor, BarrierAccelerationStructureDescriptor>;

struct RenderPassColorAttachmentDescriptor {
    TextureFormat Format{TextureFormat::UNKNOWN};
    uint32_t SampleCount{1};
    LoadAction Load{LoadAction::DontCare};
    StoreAction Store{StoreAction::Store};

    friend bool operator==(const RenderPassColorAttachmentDescriptor&, const RenderPassColorAttachmentDescriptor&) noexcept = default;
};

struct RenderPassDepthStencilAttachmentDescriptor {
    TextureFormat Format{TextureFormat::UNKNOWN};
    uint32_t SampleCount{1};
    LoadAction DepthLoad{};
    StoreAction DepthStore{};
    LoadAction StencilLoad{};
    StoreAction StencilStore{};

    friend bool operator==(const RenderPassDepthStencilAttachmentDescriptor&, const RenderPassDepthStencilAttachmentDescriptor&) noexcept = default;
};

struct RenderPassDescriptor {
    std::span<const RenderPassColorAttachmentDescriptor> ColorAttachments{};
    std::optional<RenderPassDepthStencilAttachmentDescriptor> DepthStencilAttachment{};
};

struct FramebufferDescriptor {
    RenderPass* Pass{nullptr};
    std::span<TextureView* const> ColorAttachments{};
    TextureView* DepthStencilAttachment{nullptr};
    uint32_t Width{0};
    uint32_t Height{0};
    uint32_t Layers{1};
};

struct RenderPassBeginDescriptor {
    RenderPass* Pass{nullptr};
    Framebuffer* Target{nullptr};
    std::span<const ColorClearValue> ColorClearValues{};
    std::optional<DepthStencilClearValue> DepthStencilClearValue{};
    std::string_view Name{};
};

struct TextureDescriptor {
    TextureDimension Dim{TextureDimension::UNKNOWN};
    uint32_t Width{0};
    uint32_t Height{0};
    uint32_t DepthOrArraySize{0};
    uint32_t MipLevels{0};
    uint32_t SampleCount{0};
    TextureFormat Format{TextureFormat::UNKNOWN};
    MemoryType Memory{MemoryType::Device};
    TextureUses Usage{TextureUse::UNKNOWN};
    ResourceHints Hints{ResourceHint::None};
};

struct TextureViewDescriptor {
    Texture* Target{nullptr};
    TextureDimension Dim{TextureDimension::UNKNOWN};
    TextureFormat Format{TextureFormat::UNKNOWN};
    SubresourceRange Range{};
    TextureViewUsages Usage{TextureViewUsage::UNKNOWN};
};

struct BufferDescriptor {
    uint64_t Size{0};
    MemoryType Memory{};
    BufferUses Usage{BufferUse::UNKNOWN};
    ResourceHints Hints{};
};

struct BindlessArrayDescriptor {
    uint32_t Size{0};
    BindlessSlotType SlotType{BindlessSlotType::Multiple};
};

enum class DescriptorPoolLifetime : uint8_t {
    Persistent,
    PerFlight,
};

/// Capacity is explicit so the caller, rather than the backend, owns descriptor
/// allocation policy. Counts are totals across all binding groups in the pool.
struct DescriptorPoolDescriptor {
    uint32_t MaxBindingGroups{1};
    uint32_t MaxSampledTextures{0};
    uint32_t MaxStorageTextures{0};
    uint32_t MaxUniformBuffers{0};
    uint32_t MaxDynamicUniformBuffers{0};
    uint32_t MaxStorageBuffers{0};
    uint32_t MaxReadOnlyTexelBuffers{0};
    uint32_t MaxReadWriteTexelBuffers{0};
    uint32_t MaxSamplers{0};
    uint32_t MaxAccelerationStructures{0};
    DescriptorPoolLifetime Lifetime{DescriptorPoolLifetime::Persistent};
};

struct BufferRange {
    uint64_t Offset{0};
    uint64_t Size{0};

    static constexpr uint64_t All() noexcept {
        return std::numeric_limits<uint64_t>::max();
    }

    static constexpr BufferRange AllRange() noexcept {
        return BufferRange{0, BufferRange::All()};
    }
};

struct BufferBindingDescriptor {
    Buffer* Target{nullptr};
    BufferRange Range{};
    uint32_t Stride{0};
    TextureFormat Format{TextureFormat::UNKNOWN};
    BufferViewUsage Usage{BufferViewUsage::ReadOnlyStorage};
};

struct AccelerationStructureViewDescriptor {
    AccelerationStructure* Target{nullptr};
};

using ShaderReflectionDesc = std::variant<HlslShaderDesc, SpirvShaderDesc>;

struct ShaderDescriptor {
    std::span<const byte> Source{};
    ShaderBlobCategory Category{};
    ShaderStages Stages{ShaderStage::UNKNOWN};
    std::optional<ShaderReflectionDesc> Reflection{};
};

struct ShaderParameterInfo {
    string Name{};
    ShaderParameterKind Kind{ShaderParameterKind::UNKNOWN};
    ShaderStages Stages{ShaderStage::UNKNOWN};
    ResourceBindType Type{ResourceBindType::UNKNOWN};
    uint32_t Count{1};
    uint32_t ByteSize{0};
    bool IsReadOnly{true};
    bool IsBindless{false};

    friend bool operator==(const ShaderParameterInfo&, const ShaderParameterInfo&) noexcept = default;
};

struct StaticSamplerDescriptor {
    string Name{};
    SamplerDescriptor Desc{};

    friend bool operator==(const StaticSamplerDescriptor&, const StaticSamplerDescriptor&) noexcept = default;
};

struct ShaderBindingLocation {
    uint32_t Group{0};
    uint32_t Binding{0};

    friend bool operator==(const ShaderBindingLocation&, const ShaderBindingLocation&) noexcept = default;
};

struct BindingGroupLayoutEntry {
    ShaderParameterInfo Parameter{};
    uint32_t Binding{0};
    bool HasDynamicOffset{false};
    bool IsStaticSampler{false};

    friend bool operator==(const BindingGroupLayoutEntry&, const BindingGroupLayoutEntry&) noexcept = default;
};

struct BindingGroupLayout {
    uint32_t GroupIndex{0};
    vector<BindingGroupLayoutEntry> Entries{};

    friend bool operator==(const BindingGroupLayout&, const BindingGroupLayout&) noexcept = default;
};

struct PushConstantRange {
    string Name{};
    uint32_t Group{0};
    uint32_t Binding{0};
    ShaderStages Stages{ShaderStage::UNKNOWN};
    uint32_t Offset{0};
    uint32_t Size{0};

    friend bool operator==(const PushConstantRange&, const PushConstantRange&) noexcept = default;
};

/// Identifies a cbuffer binding whose byte offset is supplied when the binding
/// group is bound. Group maps to HLSL register space / Vulkan descriptor set;
/// Binding maps to the reflected shader register / Vulkan binding.
struct DynamicBufferBinding {
    uint32_t Group{0};
    uint32_t Binding{0};

    friend bool operator==(const DynamicBufferBinding&, const DynamicBufferBinding&) noexcept = default;
};

struct PushConstantBinding {
    uint32_t Group{0};
    uint32_t Binding{0};

    friend bool operator==(const PushConstantBinding&, const PushConstantBinding&) noexcept = default;
};

/// Reuses an already-created binding-group layout in a new pipeline layout.
/// The caller owns both pipeline layouts and must keep Source alive longer than
/// the layout being created. This is explicit object reuse, not a device cache.
struct BindingGroupLayoutReuse {
    uint32_t Group{0};
    PipelineLayout* Source{nullptr};
    uint32_t SourceGroup{0};
};

struct PipelineLayoutDescriptor {
    std::span<Shader*> Shaders{};
    std::span<const StaticSamplerDescriptor> StaticSamplers{};
    std::span<const BindingGroupLayout> BindingGroupLayouts{};
    std::span<const BindingGroupLayoutReuse> BindingGroupLayoutReuses{};
    std::span<const DynamicBufferBinding> DynamicBufferBindings{};
    std::span<const PushConstantBinding> PushConstantBindings{};
};

struct VertexElement {
    uint64_t Offset{0};
    std::string_view Semantic{};
    uint32_t SemanticIndex{0};
    VertexFormat Format{VertexFormat::UNKNOWN};
    uint32_t Location{0};
};

struct VertexBufferLayout {
    uint64_t ArrayStride{0};
    VertexStepMode StepMode{};
    std::span<const VertexElement> Elements{};
};

struct PrimitiveState {
    PrimitiveTopology Topology{};
    FrontFace FaceClockwise{};
    CullMode Cull{};
    PolygonMode Poly{};
    std::optional<IndexFormat> StripIndexFormat{};
    bool UnclippedDepth{false};
    bool Conservative{false};

    constexpr static PrimitiveState Default() noexcept {
        return {
            PrimitiveTopology::TriangleList,
            FrontFace::CW,
            CullMode::Back,
            PolygonMode::Fill,
            std::nullopt,
            true,
            false};
    }

    friend bool operator==(const PrimitiveState&, const PrimitiveState&) = default;
};

struct StencilFaceState {
    CompareFunction Compare{};
    StencilOperation FailOp{};
    StencilOperation DepthFailOp{};
    StencilOperation PassOp{};

    friend bool operator==(const StencilFaceState&, const StencilFaceState&) = default;
};

struct StencilState {
    StencilFaceState Front{};
    StencilFaceState Back{};
    uint32_t ReadMask{0};
    uint32_t WriteMask{0};

    constexpr static uint32_t DefaultMask = 0xFF;

    constexpr static StencilState Default() noexcept {
        return {
            {
                CompareFunction::Always,
                StencilOperation::Keep,
                StencilOperation::Keep,
                StencilOperation::Keep,
            },
            {
                CompareFunction::Always,
                StencilOperation::Keep,
                StencilOperation::Keep,
                StencilOperation::Keep,
            },
            DefaultMask,
            DefaultMask};
    }

    friend bool operator==(const StencilState&, const StencilState&) = default;
};

struct DepthBiasState {
    int32_t Constant{0};
    float SlopScale{0.0f};
    float Clamp{0.0f};

    friend bool operator==(const DepthBiasState&, const DepthBiasState&) = default;
};

struct DepthStencilState {
    TextureFormat Format{TextureFormat::UNKNOWN};
    CompareFunction DepthCompare{};
    DepthBiasState DepthBias{};
    std::optional<StencilState> Stencil{};
    bool DepthWriteEnable{false};

    constexpr static DepthStencilState Default() noexcept {
        return {
            TextureFormat::D32_FLOAT,
            CompareFunction::Less,
            {
                0,
                0.0f,
                0.0f,
            },
            std::nullopt,
            true};
    }

    friend bool operator==(const DepthStencilState&, const DepthStencilState&) = default;
};

struct MultiSampleState {
    uint32_t Count{0};
    uint64_t Mask{0};
    bool AlphaToCoverageEnable{false};

    constexpr static MultiSampleState Default() noexcept {
        return {
            1,
            0xFFFFFFFF,
            false};
    }

    friend bool operator==(const MultiSampleState&, const MultiSampleState&) = default;
};

struct BlendComponent {
    BlendFactor Src{};
    BlendFactor Dst{};
    BlendOperation Op{};

    friend bool operator==(const BlendComponent&, const BlendComponent&) = default;
};

struct BlendState {
    BlendComponent Color{};
    BlendComponent Alpha{};

    constexpr static BlendState Default() noexcept {
        return {
            {BlendFactor::One,
             BlendFactor::Zero,
             BlendOperation::Add},
            {BlendFactor::One,
             BlendFactor::Zero,
             BlendOperation::Add}};
    }

    friend bool operator==(const BlendState&, const BlendState&) = default;
};

struct ColorTargetState {
    TextureFormat Format{TextureFormat::UNKNOWN};
    std::optional<BlendState> Blend{};
    ColorWrites WriteMask{};

    constexpr static ColorTargetState Default(TextureFormat format) noexcept {
        return {
            format,
            std::nullopt,
            ColorWrite::All};
    }

    friend bool operator==(const ColorTargetState&, const ColorTargetState&) = default;
};

struct ShaderEntry {
    Shader* Target{nullptr};
    std::string_view EntryPoint{};
};

struct GraphicsPipelineStateDescriptor {
    PipelineLayout* PipelineLayout{nullptr};
    std::optional<ShaderEntry> VS{};
    std::optional<ShaderEntry> PS{};
    std::span<const VertexBufferLayout> VertexLayouts{};
    PrimitiveState Primitive{};
    std::optional<DepthStencilState> DepthStencil{};
    MultiSampleState MultiSample{};
    std::span<const ColorTargetState> ColorTargets{};
    RenderPass* CompatibleRenderPass{nullptr};
};

struct ComputePipelineStateDescriptor {
    PipelineLayout* PipelineLayout{nullptr};
    ShaderEntry CS{};
};

struct RayTracingTrianglesDescriptor {
    Buffer* VertexBuffer{nullptr};
    uint64_t VertexOffset{0};
    uint32_t VertexStride{0};
    uint32_t VertexCount{0};
    VertexFormat VertexFmt{VertexFormat::UNKNOWN};
    Buffer* IndexBuffer{nullptr};
    uint64_t IndexOffset{0};
    uint32_t IndexCount{0};
    IndexFormat IndexFmt{IndexFormat::UINT32};
    Buffer* TransformBuffer{nullptr};
    uint64_t TransformOffset{0};
};

struct RayTracingAABBsDescriptor {
    Buffer* Target{nullptr};
    uint64_t Offset{0};
    uint32_t Count{0};
    uint32_t Stride{0};
};

struct RayTracingGeometryDesc {
    std::variant<RayTracingTrianglesDescriptor, RayTracingAABBsDescriptor> Geometry{};
    bool Opaque{true};
};

struct RayTracingInstanceDescriptor {
    Eigen::Matrix4f Transform{Eigen::Matrix4f::Identity()};
    uint32_t InstanceID{0};
    uint32_t InstanceMask{0xFF};
    uint32_t InstanceContributionToHitGroupIndex{0};
    AccelerationStructure* Blas{nullptr};
    bool ForceOpaque{false};
    bool ForceNoOpaque{false};
};

struct AccelerationStructureDescriptor {
    AccelerationStructureType Type{AccelerationStructureType::BottomLevel};
    uint32_t MaxGeometryCount{0};
    uint32_t MaxInstanceCount{0};
    AccelerationStructureBuildFlags Flags{AccelerationStructureBuildFlag::None};
};

struct BuildBottomLevelASDescriptor {
    AccelerationStructure* Target{nullptr};
    std::span<const RayTracingGeometryDesc> Geometries{};
    Buffer* ScratchBuffer{nullptr};
    uint64_t ScratchOffset{0};
    uint64_t ScratchSize{0};
    AccelerationStructureBuildMode Mode{AccelerationStructureBuildMode::Build};
};

struct BuildTopLevelASDescriptor {
    AccelerationStructure* Target{nullptr};
    std::span<const RayTracingInstanceDescriptor> Instances{};
    Buffer* ScratchBuffer{nullptr};
    uint64_t ScratchOffset{0};
    uint64_t ScratchSize{0};
    AccelerationStructureBuildMode Mode{AccelerationStructureBuildMode::Build};
};

struct RayTracingShaderEntry {
    Shader* Target{nullptr};
    std::string_view EntryPoint{};
    ShaderStage Stage{ShaderStage::UNKNOWN};
};

struct RayTracingHitGroupDescriptor {
    std::string_view Name{};
    std::optional<RayTracingShaderEntry> ClosestHit{};
    std::optional<RayTracingShaderEntry> AnyHit{};
    std::optional<RayTracingShaderEntry> Intersection{};
};

struct RayTracingPipelineStateDescriptor {
    PipelineLayout* PipelineLayout{nullptr};
    std::span<const RayTracingShaderEntry> ShaderEntries{};
    std::span<const RayTracingHitGroupDescriptor> HitGroups{};
    uint32_t MaxRecursionDepth{1};
    uint32_t MaxPayloadSize{0};
    uint32_t MaxAttributeSize{0};
};

struct ShaderBindingTableRequirements {
    uint32_t HandleSize{0};
    uint32_t HandleAlignment{0};
    uint32_t BaseAlignment{0};
};

struct ShaderBindingTableBuildEntry {
    ShaderBindingTableEntryType Type{ShaderBindingTableEntryType::RayGen};
    std::string_view ShaderName{};
    std::span<const byte> LocalData{};
    uint32_t RecordIndex{0};
};

struct ShaderBindingTableDescriptor {
    RayTracingPipelineState* Pipeline{nullptr};
    uint32_t RayGenCount{1};
    uint32_t MissCount{0};
    uint32_t HitGroupCount{0};
    uint32_t CallableCount{0};
    uint32_t MaxLocalDataSize{0};
};

struct ShaderBindingTableRegion {
    Buffer* Target{nullptr};
    uint64_t Offset{0};
    uint64_t Size{0};
    uint64_t Stride{0};
};

struct ShaderBindingTableRegions {
    ShaderBindingTableRegion RayGen{};
    ShaderBindingTableRegion Miss{};
    ShaderBindingTableRegion HitGroup{};
    std::optional<ShaderBindingTableRegion> Callable{};
};

struct TraceRaysDescriptor {
    ShaderBindingTable* Sbt{nullptr};
    ShaderBindingTableRegion RayGen{};
    ShaderBindingTableRegion Miss{};
    ShaderBindingTableRegion HitGroup{};
    std::optional<ShaderBindingTableRegion> Callable{};
    uint32_t Width{1};
    uint32_t Height{1};
    uint32_t Depth{1};
};

struct VertexBufferView {
    Buffer* Target{nullptr};
    uint64_t Offset{0};
    uint64_t Size{0};
};

struct IndexBufferView {
    Buffer* Target{nullptr};
    uint32_t Offset{0};
    uint32_t Stride{0};
};

/// Binary-compatible with D3D12_DRAW_ARGUMENTS and VkDrawIndirectCommand.
struct DrawIndirectArguments {
    uint32_t VertexCount{0};
    uint32_t InstanceCount{0};
    uint32_t FirstVertex{0};
    uint32_t FirstInstance{0};
};

/// Binary-compatible with D3D12_DRAW_INDEXED_ARGUMENTS and VkDrawIndexedIndirectCommand.
struct DrawIndexedIndirectArguments {
    uint32_t IndexCount{0};
    uint32_t InstanceCount{0};
    uint32_t FirstIndex{0};
    int32_t VertexOffset{0};
    uint32_t FirstInstance{0};
};

/// Binary-compatible with D3D12_DISPATCH_ARGUMENTS and VkDispatchIndirectCommand.
struct DispatchIndirectArguments {
    uint32_t GroupCountX{0};
    uint32_t GroupCountY{0};
    uint32_t GroupCountZ{0};
};

static_assert(sizeof(DrawIndirectArguments) == 16);
static_assert(sizeof(DrawIndexedIndirectArguments) == 20);
static_assert(sizeof(DispatchIndirectArguments) == 12);

struct DeviceDetail {
    string GpuName{};
    uint32_t CBufferAlignment{0};
    uint32_t TextureDataPitchAlignment{1};
    uint64_t TextureDataPlacementAlignment{1};
    uint64_t VramBudget{0};
    uint32_t MaxVertexInputBindings{0};
    uint32_t MaxRayRecursionDepth{0};
    uint32_t ShaderTableAlignment{0};
    uint32_t AccelerationStructureAlignment{0};
    uint32_t AccelerationStructureScratchAlignment{0};
    bool IsUMA{false};
    bool IsBindlessArraySupported{false};
    bool IsRayTracingSupported{false};
    bool IsLayeredRenderingFromVertexShaderSupported{false};
};

class Device : public enable_shared_from_this<Device>, public RenderBase {
public:
    virtual ~Device() noexcept = default;

    RenderObjectTags GetTag() const noexcept final { return RenderObjectTag::Device; }

    virtual RenderBackend GetBackend() noexcept = 0;

    virtual DeviceDetail GetDetail() const noexcept = 0;

    virtual Nullable<CommandQueue*> GetCommandQueue(QueueType type, uint32_t slot = 0) noexcept = 0;

    virtual Nullable<unique_ptr<CommandBuffer>> CreateCommandBuffer(CommandQueue* queue) noexcept = 0;

    virtual Nullable<unique_ptr<Fence>> CreateFence() noexcept = 0;

    virtual Nullable<unique_ptr<QueryPool>> CreateQueryPool(const QueryPoolDescriptor& desc) noexcept = 0;

    virtual Nullable<unique_ptr<SwapChain>> CreateSwapChain(const SwapChainDescriptor& desc) noexcept = 0;

    virtual Nullable<unique_ptr<Buffer>> CreateBuffer(const BufferDescriptor& desc) noexcept = 0;

    virtual Nullable<unique_ptr<Texture>> CreateTexture(const TextureDescriptor& desc) noexcept = 0;

    virtual Nullable<unique_ptr<TextureView>> CreateTextureView(const TextureViewDescriptor& desc) noexcept = 0;

    virtual Nullable<unique_ptr<RenderPass>> CreateRenderPass(const RenderPassDescriptor& desc) noexcept = 0;

    virtual Nullable<unique_ptr<Framebuffer>> CreateFramebuffer(const FramebufferDescriptor& desc) noexcept = 0;

    virtual Nullable<unique_ptr<Shader>> CreateShader(const ShaderDescriptor& desc) noexcept = 0;

    virtual Nullable<unique_ptr<PipelineLayout>> CreatePipelineLayout(const PipelineLayoutDescriptor& desc) noexcept = 0;

    virtual Nullable<unique_ptr<DescriptorPool>> CreateDescriptorPool(const DescriptorPoolDescriptor& desc) noexcept = 0;

    /// Creates one independently bindable register-space/descriptor-set group.
    virtual Nullable<unique_ptr<BindingGroup>> CreateBindingGroup(
        DescriptorPool* pool,
        PipelineLayout* layout,
        uint32_t groupIndex) noexcept = 0;

    virtual Nullable<unique_ptr<GraphicsPipelineState>> CreateGraphicsPipelineState(const GraphicsPipelineStateDescriptor& desc) noexcept = 0;

    virtual Nullable<unique_ptr<ComputePipelineState>> CreateComputePipelineState(const ComputePipelineStateDescriptor& desc) noexcept = 0;

    virtual Nullable<unique_ptr<AccelerationStructure>> CreateAccelerationStructure(const AccelerationStructureDescriptor& desc) noexcept = 0;

    virtual Nullable<unique_ptr<AccelerationStructureView>> CreateAccelerationStructureView(const AccelerationStructureViewDescriptor& desc) noexcept = 0;

    virtual Nullable<unique_ptr<RayTracingPipelineState>> CreateRayTracingPipelineState(const RayTracingPipelineStateDescriptor& desc) noexcept = 0;

    virtual Nullable<unique_ptr<ShaderBindingTable>> CreateShaderBindingTable(const ShaderBindingTableDescriptor& desc) noexcept = 0;

    virtual Nullable<unique_ptr<Sampler>> CreateSampler(const SamplerDescriptor& desc) noexcept = 0;

    virtual Nullable<unique_ptr<BindlessArray>> CreateBindlessArray(const BindlessArrayDescriptor& desc) noexcept = 0;

    static Nullable<shared_ptr<Device>> Create(const DeviceDescriptor& desc);
};

class CommandQueue : public RenderBase {
public:
    virtual ~CommandQueue() noexcept = default;

    RenderObjectTags GetTag() const noexcept final { return RenderObjectTag::CmdQueue; }

    virtual void Submit(const CommandQueueSubmitDescriptor& desc) noexcept = 0;

    virtual void Wait() noexcept = 0;

    virtual QueueType GetQueueType() const noexcept = 0;
};

class CommandBuffer : public RenderBase, public IDebugName {
public:
    virtual ~CommandBuffer() noexcept = default;

    RenderObjectTags GetTag() const noexcept final { return RenderObjectTag::CmdBuffer; }

    virtual void Begin() noexcept = 0;

    virtual void End() noexcept = 0;

    virtual void ResourceBarrier(std::span<const ResourceBarrierDescriptor> barriers) noexcept = 0;

    virtual Nullable<unique_ptr<GraphicsCommandEncoder>> BeginRenderPass(const RenderPassBeginDescriptor& desc) noexcept = 0;

    virtual void EndRenderPass(unique_ptr<GraphicsCommandEncoder> encoder) noexcept = 0;

    virtual Nullable<unique_ptr<ComputeCommandEncoder>> BeginComputePass() noexcept = 0;

    virtual void EndComputePass(unique_ptr<ComputeCommandEncoder> encoder) noexcept = 0;

    virtual Nullable<unique_ptr<RayTracingCommandEncoder>> BeginRayTracingPass() noexcept = 0;

    virtual void EndRayTracingPass(unique_ptr<RayTracingCommandEncoder> encoder) noexcept = 0;

    virtual void CopyBufferToBuffer(Buffer* dst, uint64_t dstOffset, Buffer* src, uint64_t srcOffset, uint64_t size) noexcept = 0;

    virtual void CopyBufferToTexture(Texture* dst, SubresourceRange dstRange, Buffer* src, uint64_t srcOffset) noexcept = 0;

    virtual void CopyTextureToBuffer(Buffer* dst, uint64_t dstOffset, Texture* src, SubresourceRange srcRange) noexcept = 0;

    /// Source requires CopySource usage and destination requires CopyDestination usage.
    virtual void CopyTextureToTexture(const TextureCopyDescriptor& desc) noexcept = 0;

    /// Resolves multisampled color. Source requires CopySource and destination requires CopyDestination usage.
    virtual void ResolveTexture(const TextureResolveDescriptor& desc) noexcept = 0;

    virtual void ResetQueryPool(QueryPool* pool, uint32_t firstIndex, uint32_t count) noexcept = 0;

    virtual void WriteTimestamp(const QueryTimestampDescriptor& desc) noexcept = 0;

    virtual void ResolveQueryData(const QueryResolveDescriptor& desc) noexcept = 0;
};

class QueryPool : public RenderBase, public IDebugName {
public:
    virtual ~QueryPool() noexcept = default;

    RenderObjectTags GetTag() const noexcept final { return RenderObjectTag::QueryPool; }

    virtual QueryType GetType() const noexcept = 0;

    virtual uint32_t GetCount() const noexcept = 0;

    virtual TimestampQueryCalibration GetTimestampCalibration(CommandQueue* queue) const noexcept = 0;
};

class CommandEncoder : public RenderBase {
public:
    virtual ~CommandEncoder() noexcept = default;

    RenderObjectTags GetTag() const noexcept override { return RenderObjectTag::CmdEncoder; }

    virtual CommandBuffer* GetCommandBuffer() const noexcept = 0;

    /// Binds one group. Dynamic offsets are ordered by ascending binding number
    /// among layout entries marked DynamicBufferBinding for this group.
    virtual void BindBindingGroup(
        uint32_t groupIndex,
        BindingGroup* group,
        std::span<const uint32_t> dynamicOffsets = {}) noexcept = 0;

    /// Writes one reflected push/root-constant range identified by its public
    /// register-space and binding. Forward and Shadow do not use this API.
    virtual bool SetPushConstants(
        PipelineLayout* layout,
        uint32_t groupIndex,
        uint32_t binding,
        std::span<const byte> data) noexcept = 0;
};

class GraphicsCommandEncoder : public CommandEncoder {
public:
    virtual ~GraphicsCommandEncoder() noexcept = default;

    RenderObjectTags GetTag() const noexcept final { return RenderObjectTag::GraphicsCmdEncoder; }

    virtual void SetViewport(Viewport vp) noexcept = 0;

    virtual void SetScissor(Rect rect) noexcept = 0;

    virtual void BindVertexBuffer(std::span<const VertexBufferView> vbv) noexcept = 0;

    virtual void BindIndexBuffer(IndexBufferView ibv) noexcept = 0;

    virtual void BindGraphicsPipelineState(GraphicsPipelineState* pso) noexcept = 0;

    virtual void Draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance) noexcept = 0;

    virtual void DrawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance) noexcept = 0;

    /// Executes tightly packed DrawIndirectArguments records.
    virtual void DrawIndirect(Buffer* argumentBuffer, uint64_t argumentOffset, uint32_t drawCount = 1) noexcept = 0;

    /// Executes tightly packed DrawIndexedIndirectArguments records.
    virtual void DrawIndexedIndirect(Buffer* argumentBuffer, uint64_t argumentOffset, uint32_t drawCount = 1) noexcept = 0;
};

class ComputeCommandEncoder : public CommandEncoder {
public:
    virtual ~ComputeCommandEncoder() noexcept = default;

    RenderObjectTags GetTag() const noexcept final { return RenderObjectTag::ComputeCmdEncoder; }

    virtual void BindComputePipelineState(ComputePipelineState* pso) noexcept = 0;

    virtual void Dispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) noexcept = 0;

    virtual void DispatchIndirect(Buffer* argumentBuffer, uint64_t argumentOffset) noexcept = 0;
};

class RayTracingCommandEncoder : public CommandEncoder {
public:
    virtual ~RayTracingCommandEncoder() noexcept = default;

    RenderObjectTags GetTag() const noexcept final { return RenderObjectTag::RayTracingCmdEncoder; }

    virtual void BuildBottomLevelAS(const BuildBottomLevelASDescriptor& desc) noexcept = 0;

    virtual void BuildTopLevelAS(const BuildTopLevelASDescriptor& desc) noexcept = 0;

    virtual void BindRayTracingPipelineState(RayTracingPipelineState* pso) noexcept = 0;

    virtual void TraceRays(const TraceRaysDescriptor& desc) noexcept = 0;
};

class Fence : public RenderBase, public IDebugName {
public:
    virtual ~Fence() noexcept = default;

    RenderObjectTags GetTag() const noexcept final { return RenderObjectTag::Fence; }

    virtual uint64_t GetCompletedValue() const noexcept = 0;

    virtual uint64_t GetLastSignaledValue() const noexcept = 0;

    virtual void Wait() noexcept = 0;

    virtual void Wait(uint64_t value) noexcept = 0;
};

class SwapChainSyncObject : public RenderBase {
public:
    virtual ~SwapChainSyncObject() noexcept = default;

    RenderObjectTags GetTag() const noexcept final { return RenderObjectTag::UNKNOWN; }
};

class SwapChain : public RenderBase {
public:
    virtual ~SwapChain() noexcept = default;

    RenderObjectTags GetTag() const noexcept final { return RenderObjectTag::SwapChain; }

    virtual SwapChainAcquireResult AcquireNext(uint64_t timeoutMs = std::numeric_limits<uint64_t>::max()) noexcept = 0;

    virtual SwapChainPresentResult Present(SwapChainFrame&& frame) noexcept = 0;

    virtual bool Recreate(uint32_t width, uint32_t height, TextureFormat format, PresentMode presentMode) noexcept = 0;

    virtual uint32_t GetBackBufferCount() const noexcept = 0;

    virtual SwapChainDescriptor GetDesc() const noexcept = 0;

protected:
    static SwapChainFrame MakeFrame(
        SwapChain* owner,
        uint64_t token,
        Texture* backBuffer,
        uint32_t backBufferIndex,
        SwapChainSyncObject* waitToDraw,
        SwapChainSyncObject* readyToPresent) noexcept;

    static bool ValidateFrame(const SwapChainFrame& frame, const SwapChain* expectedOwner, uint64_t expectedToken) noexcept;

    static void InvalidateFrame(SwapChainFrame& frame) noexcept;
};

class Resource : public RenderBase, public IDebugName {
public:
    virtual ~Resource() noexcept = default;
};

class ResourceView : public RenderBase, public IDebugName {
public:
    virtual ~ResourceView() noexcept = default;
};

class Buffer : public Resource {
public:
    virtual ~Buffer() noexcept = default;

    RenderObjectTags GetTag() const noexcept final { return RenderObjectTag::Buffer; }

    virtual void* Map(uint64_t offset, uint64_t size) noexcept = 0;

    /// For ResourceHint::PersistentMap, flushes the range as required while
    /// keeping the CPU mapping valid for the buffer lifetime.
    virtual void Unmap(uint64_t offset, uint64_t size) noexcept = 0;

    virtual BufferDescriptor GetDesc() const noexcept = 0;
};

class Texture : public Resource {
public:
    virtual ~Texture() noexcept = default;

    RenderObjectTags GetTag() const noexcept final { return RenderObjectTag::Texture; }

    virtual TextureDescriptor GetDesc() const noexcept = 0;
};

class TextureView : public ResourceView {
public:
    virtual ~TextureView() noexcept = default;

    RenderObjectTags GetTag() const noexcept final { return RenderObjectTag::TextureView; }
};

class AccelerationStructureView : public ResourceView {
public:
    virtual ~AccelerationStructureView() noexcept = default;

    RenderObjectTags GetTag() const noexcept final { return RenderObjectTag::AccelerationStructureView; }
};

class RenderPass : public RenderBase, public IDebugName {
public:
    virtual ~RenderPass() noexcept = default;

    RenderObjectTags GetTag() const noexcept final { return RenderObjectTag::RenderPass; }

    RenderPassDescriptor GetDesc() const noexcept;

protected:
    explicit RenderPass(const RenderPassDescriptor& desc);

private:
    vector<RenderPassColorAttachmentDescriptor> _colorAttachments;
    std::optional<RenderPassDepthStencilAttachmentDescriptor> _depthStencilAttachment;
};

class Framebuffer : public RenderBase, public IDebugName {
public:
    virtual ~Framebuffer() noexcept = default;

    RenderObjectTags GetTag() const noexcept final { return RenderObjectTag::Framebuffer; }

    FramebufferDescriptor GetDesc() const noexcept;

protected:
    explicit Framebuffer(const FramebufferDescriptor& desc);

private:
    RenderPass* _pass{nullptr};
    vector<TextureView*> _colorAttachments;
    TextureView* _depthStencilAttachment{nullptr};
    uint32_t _width{0};
    uint32_t _height{0};
    uint32_t _layers{1};
};

class Shader : public RenderBase {
public:
    virtual ~Shader() noexcept = default;

    RenderObjectTags GetTag() const noexcept final { return RenderObjectTag::Shader; }

    virtual ShaderStages GetStages() const noexcept = 0;

    virtual Nullable<const ShaderReflectionDesc*> GetReflection() const noexcept = 0;

    // 缓存层分配的稳定身份. 未经缓存直接创建的 Shader 为 Guid::Empty(),
    // 不能参与 PSO 缓存 key (BuildKey 会拒绝 Empty).
    Guid GetGuid() const noexcept { return _guid; }
    void SetGuid(Guid guid) noexcept { _guid = guid; }

protected:
    Guid _guid{};
};

class PipelineLayout : public RenderBase, public IDebugName {
public:
    virtual ~PipelineLayout() noexcept = default;

    RenderObjectTags GetTag() const noexcept final { return RenderObjectTag::PipelineLayout; }

    virtual vector<ShaderParameterInfo> GetParameters() const noexcept = 0;

    virtual Nullable<const ShaderParameterInfo*> FindParameter(std::string_view name) const noexcept = 0;

    virtual std::optional<ShaderBindingLocation> FindBindingLocation(std::string_view name) const noexcept = 0;

    virtual vector<BindingGroupLayout> GetBindingGroupLayouts() const noexcept = 0;

    virtual vector<PushConstantRange> GetPushConstantRanges() const noexcept = 0;

    // Optional identity assigned by an owning runtime resource library.
    Guid GetGuid() const noexcept { return _guid; }
    void SetGuid(Guid guid) noexcept { _guid = guid; }

protected:
    Guid _guid{};
};

class PipelineState : public RenderBase, public IDebugName {
public:
    virtual ~PipelineState() noexcept = default;

    RenderObjectTags GetTag() const noexcept override { return RenderObjectTag::PipelineState; }
};

class GraphicsPipelineState : public PipelineState {
public:
    virtual ~GraphicsPipelineState() noexcept = default;

    RenderObjectTags GetTag() const noexcept final { return RenderObjectTag::GraphicsPipelineState; }
};

class ComputePipelineState : public PipelineState {
public:
    virtual ~ComputePipelineState() noexcept = default;

    RenderObjectTags GetTag() const noexcept final { return RenderObjectTag::ComputePipelineState; }
};

class RayTracingPipelineState : public PipelineState {
public:
    virtual ~RayTracingPipelineState() noexcept = default;

    RenderObjectTags GetTag() const noexcept final { return RenderObjectTag::RayTracingPipelineState; }

    virtual ShaderBindingTableRequirements GetShaderBindingTableRequirements() const noexcept = 0;

    virtual std::optional<vector<byte>> GetShaderBindingTableHandle(std::string_view shaderName) const noexcept = 0;
};

class ShaderBindingTable : public RenderBase, public IDebugName {
public:
    virtual ~ShaderBindingTable() noexcept = default;

    RenderObjectTags GetTag() const noexcept final { return RenderObjectTag::ShaderBindingTable; }

    virtual bool Build(std::span<const ShaderBindingTableBuildEntry> entries) noexcept = 0;

    virtual bool IsBuilt() const noexcept = 0;

    virtual ShaderBindingTableRegions GetRegions() const noexcept = 0;
};

class AccelerationStructure : public Resource {
public:
    virtual ~AccelerationStructure() noexcept = default;

    RenderObjectTags GetTag() const noexcept final { return RenderObjectTag::AccelerationStructure; }
};

class DescriptorPool : public RenderBase, public IDebugName {
public:
    virtual ~DescriptorPool() noexcept = default;

    RenderObjectTags GetTag() const noexcept final { return RenderObjectTag::DescriptorPool; }

    /// Reset is only valid after all binding groups allocated from this pool
    /// have been destroyed and their GPU work has completed.
    virtual bool Reset() noexcept = 0;

    virtual DescriptorPoolDescriptor GetDesc() const noexcept = 0;

    virtual uint32_t GetAllocatedBindingGroupCount() const noexcept = 0;
};

/// A single independently bindable resource group/register space.
class BindingGroup : public RenderBase, public IDebugName {
public:
    virtual ~BindingGroup() noexcept = default;

    RenderObjectTags GetTag() const noexcept final { return RenderObjectTag::BindingGroup; }

    virtual void Reset() noexcept = 0;

    virtual PipelineLayout* GetPipelineLayout() const noexcept = 0;

    virtual uint32_t GetGroupIndex() const noexcept = 0;

    virtual bool SetResource(uint32_t binding, ResourceView* view, uint32_t arrayIndex = 0) noexcept = 0;

    virtual bool SetResource(uint32_t binding, const BufferBindingDescriptor& desc, uint32_t arrayIndex = 0) noexcept = 0;

    virtual bool SetSampler(uint32_t binding, Sampler* sampler, uint32_t arrayIndex = 0) noexcept = 0;

    virtual bool SetBindlessArray(uint32_t binding, BindlessArray* array) noexcept = 0;
};

class Sampler : public RenderBase, public IDebugName {
public:
    virtual ~Sampler() noexcept = default;

    RenderObjectTags GetTag() const noexcept final { return RenderObjectTag::Sampler; }
};

class BindlessArray : public RenderBase, public IDebugName {
public:
    virtual ~BindlessArray() noexcept = default;

    RenderObjectTags GetTag() const noexcept final { return RenderObjectTag::BindlessArray; }

    virtual void SetBuffer(uint32_t slot, const BufferBindingDescriptor& desc) noexcept = 0;

    virtual void SetTexture(uint32_t slot, TextureView* texView, Sampler* sampler) noexcept = 0;
};

class InstanceVulkan : public RenderBase {
public:
    InstanceVulkan() noexcept = default;
    InstanceVulkan(const InstanceVulkan&) = delete;
    InstanceVulkan(InstanceVulkan&&) = delete;
    InstanceVulkan& operator=(const InstanceVulkan&) = delete;
    InstanceVulkan& operator=(InstanceVulkan&&) = delete;
    virtual ~InstanceVulkan() noexcept = default;

    RenderObjectTags GetTag() const noexcept final { return RenderObjectTag::VkInstance; }

    virtual vector<VulkanPhysicalDeviceInfo> GetPhysicalDevices() const noexcept = 0;

    virtual std::optional<uint32_t> SelectHighPerformancePhysicalDevice() const noexcept = 0;

    static Nullable<InstanceVulkan*> InitEnv(const VulkanInstanceDescriptor& desc);
    static void ShutdownEnv() noexcept;
};

class DXGIFactory : public RenderBase {
public:
    DXGIFactory() noexcept = default;
    DXGIFactory(const DXGIFactory&) = delete;
    DXGIFactory(DXGIFactory&&) = delete;
    DXGIFactory& operator=(const DXGIFactory&) = delete;
    DXGIFactory& operator=(DXGIFactory&&) = delete;
    virtual ~DXGIFactory() noexcept = default;

    RenderObjectTags GetTag() const noexcept final { return RenderObjectTag::DXGIFactory; }

    virtual vector<DXGIAdapterInfo> GetAdapters() const noexcept = 0;

    virtual std::optional<uint32_t> SelectHighPerformanceAdapter() const noexcept = 0;

    static Nullable<unique_ptr<DXGIFactory>> Create(const DXGIFactoryDescriptor& desc);
};

// --------------------------- Utility Functions ---------------------------
bool IsDepthStencilFormat(TextureFormat format) noexcept;
bool IsUintFormat(TextureFormat format) noexcept;
bool IsSintFormat(TextureFormat format) noexcept;
uint32_t GetVertexFormatSizeInBytes(VertexFormat format) noexcept;
uint32_t GetIndexFormatSizeInBytes(IndexFormat format) noexcept;
IndexFormat SizeInBytesToIndexFormat(uint32_t size) noexcept;
uint32_t GetTextureFormatBytesPerPixel(TextureFormat format) noexcept;
ResourceBindType BufferViewUsageToResourceBindType(BufferViewUsage usage) noexcept;
bool IsGraphicsPipelineCompatibleWithRenderPass(
    const GraphicsPipelineStateDescriptor& pipeline,
    const RenderPass& renderPass) noexcept;
// -------------------------------------------------------------------------

std::string_view format_as(RenderBackend v) noexcept;
std::string_view format_as(TextureFormat v) noexcept;
std::string_view format_as(QueueType v) noexcept;
std::string_view format_as(ShaderBlobCategory v) noexcept;
std::string_view format_as(VertexFormat v) noexcept;
std::string_view format_as(PolygonMode v) noexcept;
std::string_view format_as(TextureDimension v) noexcept;
std::string_view format_as(BufferState v) noexcept;
std::string_view format_as(TextureState v) noexcept;
std::string_view format_as(TextureViewUsage v) noexcept;
std::string_view format_as(BufferViewUsage v) noexcept;
std::string_view format_as(ResourceBindType v) noexcept;
std::string_view format_as(AccelerationStructureType v) noexcept;
std::string_view format_as(AccelerationStructureBuildMode v) noexcept;
std::string_view format_as(AccelerationStructureBuildFlag v) noexcept;
std::string_view format_as(RenderObjectTag v) noexcept;
std::string_view format_as(PresentMode v) noexcept;
std::string_view format_as(ShaderStage v) noexcept;
std::string_view format_as(BindlessSlotType v) noexcept;
std::string_view format_as(SwapChainStatus v) noexcept;

}  // namespace radray::render
