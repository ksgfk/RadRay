#pragma once

#include <array>
#include <functional>
#include <limits>
#include <variant>
#include <optional>
#include <span>
#include <string_view>

#include <radray/types.h>
#include <radray/nullable.h>
#include <radray/enum_flags.h>
#include <radray/basic_math.h>

namespace radray::render {

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
    Graphics = Vertex | Pixel,
};

enum class ShaderBlobCategory : int32_t {
    DXIL,
    SPIRV,
    MSL,
    METALLIB,
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
    Indirect = UnorderedAccess << 1
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
    HostWrite = HostRead << 1
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

enum class TextureViewUsage : uint32_t {
    UNKNOWN,
    Resource,
    RenderTarget,
    DepthRead,
    DepthWrite,
    UnorderedAccess,
};

enum class ResourceHint : uint32_t {
    None = 0x0,
    Dedicated = 0x1,
    External = Dedicated << 1,
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

enum class PresentMode : int32_t {
    FIFO,
    Mailbox,
    Immediate
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
    Compute
};

enum class PhysicalDeviceType : int32_t {
    Other,
    IntegratedGpu,
    DiscreteGpu,
    VirtualGpu,
    Cpu,
};

enum class ShaderParameterBindingType : int32_t {
    UNKNOWN,
    CBuffer,
    Buffer,
    RWBuffer,
    TexelBuffer,
    RWTexelBuffer,
    Texture,
    RWTexture,
    DynamicCBuffer,
    DynamicBuffer,
    DynamicRWBuffer,
    Sampler,
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
    PipelineState = PipelineLayout << 1,
    GraphicsPipelineState = PipelineState | (PipelineState << 1),
    ComputePipelineState = PipelineState | (PipelineState << 2),
    SwapChain = PipelineState << 3,
    Resource = SwapChain << 1,
    Buffer = Resource | (Resource << 1),
    Texture = Resource | (Resource << 2),
    ResourceView = Resource << 3,
    TextureView = ResourceView | (ResourceView << 1),
    Sampler = ResourceView << 2,
    QueryPool = Sampler << 1,
    RenderPass = QueryPool << 1,
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
struct is_flags<render::BufferState> : public std::true_type {};
template <>
struct is_flags<render::TextureState> : public std::true_type {};

namespace render {

using ShaderStages = EnumFlags<render::ShaderStage>;
using ColorWrites = EnumFlags<render::ColorWrite>;
using ResourceHints = EnumFlags<render::ResourceHint>;
using RenderObjectTags = EnumFlags<render::RenderObjectTag>;
using BufferUses = EnumFlags<render::BufferUse>;
using TextureUses = EnumFlags<render::TextureUse>;
using BufferStates = EnumFlags<render::BufferState>;
using TextureStates = EnumFlags<render::TextureState>;

}  // namespace render

}  // namespace radray

namespace radray::render {

class Device;
class CommandQueue;
class CommandBuffer;
class CommandEncoder;
class GraphicsCommandEncoder;
class ComputeCommandEncoder;
class Fence;
class SwapChain;
class SwapChainSyncObject;
class SwapChainFrame;
class Resource;
class ResourceView;
class Buffer;
class Texture;
class TextureView;
class Shader;
class PipelineLayout;
class RenderPass;
class Framebuffer;
class PipelineState;
class GraphicsPipelineState;
class ComputePipelineState;
class Sampler;
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

    friend bool operator==(const SubresourceRange&, const SubresourceRange&) noexcept = default;
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

using ResourceBarrierDescriptor = std::variant<BarrierBufferDescriptor, BarrierTextureDescriptor>;

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
    TextureViewUsage Usage{TextureViewUsage::UNKNOWN};
};

struct BufferDescriptor {
    uint64_t Size{0};
    MemoryType Memory{};
    BufferUses Usage{BufferUse::UNKNOWN};
    ResourceHints Hints{};
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

struct MappedBufferRange {
    Buffer* Target{nullptr};
    BufferRange Range{};
};

struct ShaderDescriptor {
    std::span<const byte> Source{};
    ShaderBlobCategory Category{};
    ShaderStages Stages{ShaderStage::UNKNOWN};
};

struct ShaderBindingLocation {
    uint32_t Group{0};
    uint32_t Binding{0};

    friend bool operator==(const ShaderBindingLocation&, const ShaderBindingLocation&) noexcept = default;
};

struct ShaderParameterSetLayoutEntryDescriptor {
    uint32_t Binding{0};
    ShaderParameterBindingType Type{ShaderParameterBindingType::UNKNOWN};
    uint32_t Count{0};
    ShaderStages Stages{ShaderStage::UNKNOWN};
    // When non-null, the sampler must remain valid until the Device is destroyed.
    Nullable<Sampler*> ImmutableSampler{};

    friend bool operator==(const ShaderParameterSetLayoutEntryDescriptor&, const ShaderParameterSetLayoutEntryDescriptor&) noexcept = default;
};

struct ShaderParameterSetLayoutDescriptor {
    uint32_t GroupIndex{0};
    std::span<const ShaderParameterSetLayoutEntryDescriptor> Entries{};
};

struct PushConstantDescriptor {
    ShaderBindingLocation Location{};
    uint32_t Size{0};
    ShaderStages Stages{ShaderStage::UNKNOWN};

    friend bool operator==(const PushConstantDescriptor&, const PushConstantDescriptor&) noexcept = default;
};

struct PipelineLayoutDescriptor {
    std::span<const ShaderParameterSetLayoutDescriptor> ParameterSets{};
    std::optional<PushConstantDescriptor> PushConstant{};
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
    bool DepthTestEnable{false};
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
            true,
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

struct DrawIndirectArguments {
    uint32_t VertexCount{0};
    uint32_t InstanceCount{0};
    uint32_t FirstVertex{0};
    uint32_t FirstInstance{0};
};

struct DrawIndexedIndirectArguments {
    uint32_t IndexCount{0};
    uint32_t InstanceCount{0};
    uint32_t FirstIndex{0};
    int32_t VertexOffset{0};
    uint32_t FirstInstance{0};
};

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
    uint64_t BufferCopyOffsetAlignment{1};
    uint64_t TextureDataPlacementAlignment{1};
    uint64_t VramBudget{0};
    uint32_t CBufferAlignment{0};
    uint32_t TextureDataPitchAlignment{1};
    uint32_t MaxVertexInputBindings{0};
    bool IsUMA{false};
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

    virtual void FlushMappedRanges(std::span<const MappedBufferRange> ranges) noexcept = 0;

    virtual Nullable<unique_ptr<Texture>> CreateTexture(const TextureDescriptor& desc) noexcept = 0;

    virtual Nullable<unique_ptr<TextureView>> CreateTextureView(const TextureViewDescriptor& desc) noexcept = 0;

    virtual Nullable<unique_ptr<RenderPass>> CreateRenderPass(const RenderPassDescriptor& desc) noexcept = 0;

    virtual Nullable<unique_ptr<Framebuffer>> CreateFramebuffer(const FramebufferDescriptor& desc) noexcept = 0;

    virtual Nullable<unique_ptr<Shader>> CreateShader(const ShaderDescriptor& desc) noexcept = 0;

    virtual Nullable<unique_ptr<PipelineLayout>> CreatePipelineLayout(const PipelineLayoutDescriptor& desc) noexcept = 0;

    virtual Nullable<unique_ptr<GraphicsPipelineState>> CreateGraphicsPipelineState(const GraphicsPipelineStateDescriptor& desc) noexcept = 0;

    virtual Nullable<unique_ptr<ComputePipelineState>> CreateComputePipelineState(const ComputePipelineStateDescriptor& desc) noexcept = 0;

    virtual Nullable<unique_ptr<Sampler>> CreateSampler(const SamplerDescriptor& desc) noexcept = 0;

    // Returns a Device-owned sampler. The pointer remains valid until the Device is destroyed.
    virtual Nullable<Sampler*> GetOrCreateSampler(const SamplerDescriptor& desc) noexcept = 0;

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

    virtual void CopyBufferToBuffer(Buffer* dst, uint64_t dstOffset, Buffer* src, uint64_t srcOffset, uint64_t size) noexcept = 0;

    virtual void CopyBufferToTexture(Texture* dst, SubresourceRange dstRange, Buffer* src, uint64_t srcOffset) noexcept = 0;

    virtual void CopyTextureToBuffer(Buffer* dst, uint64_t dstOffset, Texture* src, SubresourceRange srcRange) noexcept = 0;

    virtual void CopyTextureToTexture(const TextureCopyDescriptor& desc) noexcept = 0;

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

    virtual void DrawIndirect(Buffer* argumentBuffer, uint64_t argumentOffset, uint32_t drawCount = 1) noexcept = 0;

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

    virtual void Unmap() noexcept = 0;

    virtual void FlushMappedRange(BufferRange range) noexcept = 0;

    virtual void InvalidateMappedRange(BufferRange range) noexcept = 0;

    virtual BufferDescriptor GetDesc() const noexcept = 0;

    virtual Device* GetDevice() const noexcept = 0;
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

class RenderPass : public RenderBase, public IDebugName {
public:
    virtual ~RenderPass() noexcept = default;

    RenderObjectTags GetTag() const noexcept final { return RenderObjectTag::RenderPass; }

    virtual RenderPassDescriptor GetDesc() const noexcept = 0;
};

class Framebuffer : public RenderBase, public IDebugName {
public:
    virtual ~Framebuffer() noexcept = default;

    RenderObjectTags GetTag() const noexcept final { return RenderObjectTag::Framebuffer; }

    virtual FramebufferDescriptor GetDesc() const noexcept = 0;
};

class Shader : public RenderBase {
public:
    virtual ~Shader() noexcept = default;

    RenderObjectTags GetTag() const noexcept final { return RenderObjectTag::Shader; }

    virtual ShaderStages GetStages() const noexcept = 0;
};

class PipelineLayout : public RenderBase, public IDebugName {
public:
    virtual ~PipelineLayout() noexcept = default;

    RenderObjectTags GetTag() const noexcept final { return RenderObjectTag::PipelineLayout; }
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

class Sampler : public RenderBase, public IDebugName {
public:
    virtual ~Sampler() noexcept = default;

    RenderObjectTags GetTag() const noexcept final { return RenderObjectTag::Sampler; }
};

class InstanceVulkan : public RenderBase {
public:
    virtual ~InstanceVulkan() noexcept = default;

    RenderObjectTags GetTag() const noexcept final { return RenderObjectTag::VkInstance; }

    virtual vector<VulkanPhysicalDeviceInfo> GetPhysicalDevices() const noexcept = 0;

    virtual std::optional<uint32_t> SelectHighPerformancePhysicalDevice() const noexcept = 0;

    static Nullable<InstanceVulkan*> InitEnv(const VulkanInstanceDescriptor& desc);
    static void ShutdownEnv() noexcept;
};

class DXGIFactory : public RenderBase {
public:
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
bool IsDynamicShaderParameterBindingType(ShaderParameterBindingType type) noexcept;
// -------------------------------------------------------------------------

std::string_view format_as(RenderBackend v) noexcept;
std::string_view format_as(ShaderStage v) noexcept;
std::string_view format_as(ShaderBlobCategory v) noexcept;
std::string_view format_as(TextureFormat v) noexcept;
std::string_view format_as(QueueType v) noexcept;
std::string_view format_as(VertexFormat v) noexcept;
std::string_view format_as(PolygonMode v) noexcept;
std::string_view format_as(TextureDimension v) noexcept;
std::string_view format_as(BufferState v) noexcept;
std::string_view format_as(TextureState v) noexcept;
std::string_view format_as(TextureViewUsage v) noexcept;
std::string_view format_as(RenderObjectTag v) noexcept;
std::string_view format_as(PresentMode v) noexcept;
std::string_view format_as(SwapChainStatus v) noexcept;

}  // namespace radray::render

namespace std {

template <>
struct hash<radray::render::SamplerDescriptor> {
    size_t operator()(const radray::render::SamplerDescriptor& desc) const noexcept;
};

}  // namespace std

namespace radray::render {

// Non-thread-safe cache of samplers owned by a Device backend.
class SamplerCache final {
public:
    explicit SamplerCache(Device* device) noexcept;

    ~SamplerCache() noexcept;

    SamplerCache(const SamplerCache&) = delete;
    SamplerCache(SamplerCache&&) = delete;
    SamplerCache& operator=(const SamplerCache&) = delete;
    SamplerCache& operator=(SamplerCache&&) = delete;

    Nullable<Sampler*> GetOrCreate(const SamplerDescriptor& desc) noexcept;

    void Clear() noexcept;

private:
    Device* _device;
    unordered_map<SamplerDescriptor, unique_ptr<Sampler>> _cache;
};

}  // namespace radray::render
