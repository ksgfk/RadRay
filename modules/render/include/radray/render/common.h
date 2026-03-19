#pragma once

#include <array>
#include <limits>
#include <variant>
#include <optional>
#include <span>
#include <vector>

#include <radray/types.h>
#include <radray/nullable.h>
#include <radray/enum_flags.h>
#include <radray/basic_math.h>

#include <radray/render/shader/hlsl.h>
#include <radray/render/shader/spirv.h>

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
    UnorderedAccess = DepthWrite << 1
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

enum class BindingParameterKind : int32_t {
    UNKNOWN,
    Resource,
    Sampler,
    PushConstant
};

enum class SwapChainAcquireStatus : int32_t {
    Error,
    Success,
    RetryLater,
    RequireRecreate
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
    RootSignature = Shader << 1,
    PipelineState = RootSignature << 1,
    GraphicsPipelineState = PipelineState | (PipelineState << 1),
    ComputePipelineState = PipelineState | (PipelineState << 2),
    SwapChain = PipelineState << 3,
    Resource = SwapChain << 1,
    Buffer = Resource | (Resource << 1),
    Texture = Resource | (Resource << 2),
    ResourceView = Resource << 3,
    BufferView = ResourceView | (ResourceView << 1),
    TextureView = ResourceView | (ResourceView << 2),
    AccelerationStructureView = ResourceView | (ResourceView << 3),
    DescriptorSet = ResourceView << 4,
    Sampler = DescriptorSet << 1,
    BindlessArray = Sampler << 1,
    RayTracingCmdEncoder = BindlessArray << 1,
    AccelerationStructure = RayTracingCmdEncoder << 1,
    RayTracingPipelineState = AccelerationStructure << 1,
    ShaderBindingTable = RayTracingPipelineState << 1,

    VkInstance = ShaderBindingTable << 1
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
class Resource;
class ResourceView;
class Buffer;
class BufferView;
class Texture;
class TextureView;
class AccelerationStructureView;
class Shader;
class DescriptorSet;
class RootSignature;
class PipelineState;
class GraphicsPipelineState;
class ComputePipelineState;
class RayTracingPipelineState;
class ShaderBindingTable;
class AccelerationStructure;
class Sampler;
class BindlessArray;
class InstanceVulkan;

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

class VulkanInstanceDescriptor {
public:
    std::string_view AppName{};
    uint32_t AppVersion{0};
    std::string_view EngineName{};
    uint32_t EngineVersion{0};
    bool IsEnableDebugLayer{false};
    bool IsEnableGpuBasedValid{false};
    RenderLogCallback LogCallback{nullptr};
    void* LogUserData{nullptr};
};

class D3D12DeviceDescriptor {
public:
    std::optional<uint32_t> AdapterIndex{};
    bool IsEnableDebugLayer{false};
    bool IsEnableGpuBasedValid{false};
    RenderLogCallback LogCallback{nullptr};
    void* LogUserData{nullptr};
};

class MetalDeviceDescriptor {
public:
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

    // Fence 信号 — Fences 和 Values 长度必须一致
    std::span<Fence*> SignalFences{};
    std::span<uint64_t> SignalValues{};

    // GPU 侧 fence 等待 — Fences 和 Values 长度必须一致
    std::span<Fence*> WaitFences{};
    std::span<uint64_t> WaitValues{};

    // Swapchain 同步 — 支持多 surface
    std::span<SwapChainSyncObject*> WaitToExecute{};
    std::span<SwapChainSyncObject*> ReadyToPresent{};
};

struct AcquireResult {
    Nullable<Texture*> BackBuffer{nullptr};
    SwapChainSyncObject* WaitToDraw{nullptr};
    SwapChainSyncObject* ReadyToPresent{nullptr};
    int64_t NativeStatusCode{0};
    SwapChainAcquireStatus Status{SwapChainAcquireStatus::Error};
    uint32_t BackBufferIndex{std::numeric_limits<uint32_t>::max()};
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

struct ColorAttachment {
    TextureView* Target{nullptr};
    LoadAction Load{LoadAction::DontCare};
    StoreAction Store{StoreAction::Store};
    ColorClearValue ClearValue{};
};

struct DepthStencilAttachment {
    TextureView* Target{nullptr};
    LoadAction DepthLoad{};
    StoreAction DepthStore{};
    LoadAction StencilLoad{};
    StoreAction StencilStore{};
    DepthStencilClearValue ClearValue{};
};

struct RenderPassDescriptor {
    std::span<const ColorAttachment> ColorAttachments{};
    std::optional<DepthStencilAttachment> DepthStencilAttachment{};
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

struct BufferRange {
    uint64_t Offset{0};
    uint64_t Size{0};
};

struct BufferViewDescriptor {
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

struct BindingParameterId {
    uint32_t Value{0};

    constexpr BindingParameterId() noexcept = default;
    constexpr BindingParameterId(uint32_t value) noexcept : Value(value) {}

    constexpr operator uint32_t() const noexcept { return Value; }

    friend auto operator<=>(const BindingParameterId& lhs, const BindingParameterId& rhs) noexcept = default;
};

struct DescriptorSetIndex {
    uint32_t Value{0};

    constexpr DescriptorSetIndex() noexcept = default;
    constexpr DescriptorSetIndex(uint32_t value) noexcept : Value(value) {}

    constexpr operator uint32_t() const noexcept { return Value; }

    friend auto operator<=>(const DescriptorSetIndex& lhs, const DescriptorSetIndex& rhs) noexcept = default;
};

struct ResourceBindingAbi {
    DescriptorSetIndex Set{0};
    uint32_t Binding{0};
    ResourceBindType Type{ResourceBindType::UNKNOWN};
    uint32_t Count{1};
    bool IsReadOnly{true};
    bool IsBindless{false};
};

struct PushConstantBindingAbi {
    uint32_t Offset{0};
    uint32_t Size{0};
};

struct BindingParameterLayout {
    string Name{};
    BindingParameterId Id{0};
    BindingParameterKind Kind{BindingParameterKind::UNKNOWN};
    ShaderStages Stages{ShaderStage::UNKNOWN};
    std::variant<ResourceBindingAbi, PushConstantBindingAbi> Abi{};
};

class BindingLayout {
public:
    BindingLayout() noexcept = default;

    explicit BindingLayout(vector<BindingParameterLayout> parameters) noexcept
        : _parameters(std::move(parameters)) {}

    std::span<const BindingParameterLayout> GetParameters() const noexcept {
        return _parameters;
    }

    std::optional<BindingParameterId> FindParameterId(std::string_view name) const noexcept {
        for (const auto& parameter : _parameters) {
            if (parameter.Name == name) {
                return parameter.Id;
            }
        }
        return std::nullopt;
    }

    Nullable<const BindingParameterLayout*> FindParameter(BindingParameterId id) const noexcept {
        for (const auto& parameter : _parameters) {
            if (parameter.Id == id) {
                return &parameter;
            }
        }
        return nullptr;
    }

private:
    vector<BindingParameterLayout> _parameters{};
};

struct PushConstantRange {
    string Name{};
    BindingParameterId Id{0};
    ShaderStages Stages{ShaderStage::UNKNOWN};
    uint32_t Offset{0};
    uint32_t Size{0};
};

struct BindlessSetLayout {
    string Name{};
    BindingParameterId Id{0};
    DescriptorSetIndex Set{0};
    uint32_t Binding{0};
    ResourceBindType Type{ResourceBindType::UNKNOWN};
    BindlessSlotType SlotType{BindlessSlotType::Multiple};
    ShaderStages Stages{ShaderStage::UNKNOWN};
};

struct StaticSamplerDescriptor {
    string Name{};
    DescriptorSetIndex Set{0};
    uint32_t Binding{0};
    ShaderStages Stages{ShaderStage::UNKNOWN};
    SamplerDescriptor Desc{};
};

struct StaticSamplerLayout {
    string Name{};
    BindingParameterId Id{0};
    DescriptorSetIndex Set{0};
    uint32_t Binding{0};
    ShaderStages Stages{ShaderStage::UNKNOWN};
    SamplerDescriptor Desc{};
};

struct RootSignatureDescriptor {
    std::span<Shader*> Shaders{};
    std::span<const StaticSamplerDescriptor> StaticSamplers{};
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
};

struct StencilFaceState {
    CompareFunction Compare{};
    StencilOperation FailOp{};
    StencilOperation DepthFailOp{};
    StencilOperation PassOp{};
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
};

struct DepthBiasState {
    int32_t Constant{0};
    float SlopScale{0.0f};
    float Clamp{0.0f};
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
};

struct BlendComponent {
    BlendFactor Src{};
    BlendFactor Dst{};
    BlendOperation Op{};
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
};

struct ShaderEntry {
    Shader* Target{nullptr};
    std::string_view EntryPoint{};
};

struct GraphicsPipelineStateDescriptor {
    RootSignature* RootSig{nullptr};
    std::optional<ShaderEntry> VS{};
    std::optional<ShaderEntry> PS{};
    std::span<const VertexBufferLayout> VertexLayouts{};
    PrimitiveState Primitive{};
    std::optional<DepthStencilState> DepthStencil{};
    MultiSampleState MultiSample{};
    std::span<const ColorTargetState> ColorTargets{};
};

struct ComputePipelineStateDescriptor {
    RootSignature* RootSig{nullptr};
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
    RootSignature* RootSig{nullptr};
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

struct DeviceDetail {
    string GpuName{};
    uint32_t CBufferAlignment{0};
    uint32_t TextureDataPitchAlignment{1};
    uint64_t VramBudget{0};
    uint32_t MaxVertexInputBindings{0};
    uint32_t MaxRayRecursionDepth{0};
    uint32_t ShaderTableAlignment{0};
    uint32_t AccelerationStructureAlignment{0};
    uint32_t AccelerationStructureScratchAlignment{0};
    bool IsUMA{false};
    bool IsBindlessArraySupported{false};
    bool IsRayTracingSupported{false};
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

    virtual Nullable<unique_ptr<SwapChain>> CreateSwapChain(const SwapChainDescriptor& desc) noexcept = 0;

    virtual Nullable<unique_ptr<Buffer>> CreateBuffer(const BufferDescriptor& desc) noexcept = 0;

    virtual Nullable<unique_ptr<BufferView>> CreateBufferView(const BufferViewDescriptor& desc) noexcept = 0;

    virtual Nullable<unique_ptr<Texture>> CreateTexture(const TextureDescriptor& desc) noexcept = 0;

    virtual Nullable<unique_ptr<TextureView>> CreateTextureView(const TextureViewDescriptor& desc) noexcept = 0;

    virtual Nullable<unique_ptr<Shader>> CreateShader(const ShaderDescriptor& desc) noexcept = 0;

    virtual Nullable<unique_ptr<RootSignature>> CreateRootSignature(const RootSignatureDescriptor& desc) noexcept = 0;

    virtual Nullable<unique_ptr<DescriptorSet>> CreateDescriptorSet(RootSignature* rootSig, DescriptorSetIndex set) noexcept = 0;

    virtual Nullable<unique_ptr<GraphicsPipelineState>> CreateGraphicsPipelineState(const GraphicsPipelineStateDescriptor& desc) noexcept = 0;

    virtual Nullable<unique_ptr<ComputePipelineState>> CreateComputePipelineState(const ComputePipelineStateDescriptor& desc) noexcept = 0;

    virtual Nullable<unique_ptr<AccelerationStructure>> CreateAccelerationStructure(const AccelerationStructureDescriptor& desc) noexcept = 0;

    virtual Nullable<unique_ptr<AccelerationStructureView>> CreateAccelerationStructureView(const AccelerationStructureViewDescriptor& desc) noexcept = 0;

    virtual Nullable<unique_ptr<RayTracingPipelineState>> CreateRayTracingPipelineState(const RayTracingPipelineStateDescriptor& desc) noexcept = 0;

    virtual Nullable<unique_ptr<ShaderBindingTable>> CreateShaderBindingTable(const ShaderBindingTableDescriptor& desc) noexcept = 0;

    virtual Nullable<unique_ptr<Sampler>> CreateSampler(const SamplerDescriptor& desc) noexcept = 0;

    virtual Nullable<unique_ptr<BindlessArray>> CreateBindlessArray(const BindlessArrayDescriptor& desc) noexcept = 0;
};

class CommandQueue : public RenderBase {
public:
    virtual ~CommandQueue() noexcept = default;

    RenderObjectTags GetTag() const noexcept final { return RenderObjectTag::CmdQueue; }

    virtual void Submit(const CommandQueueSubmitDescriptor& desc) noexcept = 0;

    virtual void Wait() noexcept = 0;
};

class CommandBuffer : public RenderBase, IDebugName {
public:
    virtual ~CommandBuffer() noexcept = default;

    RenderObjectTags GetTag() const noexcept final { return RenderObjectTag::CmdBuffer; }

    virtual void Begin() noexcept = 0;

    virtual void End() noexcept = 0;

    virtual void ResourceBarrier(std::span<const ResourceBarrierDescriptor> barriers) noexcept = 0;

    virtual Nullable<unique_ptr<GraphicsCommandEncoder>> BeginRenderPass(const RenderPassDescriptor& desc) noexcept = 0;

    virtual void EndRenderPass(unique_ptr<GraphicsCommandEncoder> encoder) noexcept = 0;

    virtual Nullable<unique_ptr<ComputeCommandEncoder>> BeginComputePass() noexcept = 0;

    virtual void EndComputePass(unique_ptr<ComputeCommandEncoder> encoder) noexcept = 0;

    virtual Nullable<unique_ptr<RayTracingCommandEncoder>> BeginRayTracingPass() noexcept = 0;

    virtual void EndRayTracingPass(unique_ptr<RayTracingCommandEncoder> encoder) noexcept = 0;

    virtual void CopyBufferToBuffer(Buffer* dst, uint64_t dstOffset, Buffer* src, uint64_t srcOffset, uint64_t size) noexcept = 0;

    virtual void CopyBufferToTexture(Texture* dst, SubresourceRange dstRange, Buffer* src, uint64_t srcOffset) noexcept = 0;

    virtual void CopyTextureToBuffer(Buffer* dst, uint64_t dstOffset, Texture* src, SubresourceRange srcRange) noexcept = 0;
};

class CommandEncoder : public RenderBase {
public:
    virtual ~CommandEncoder() noexcept = default;

    RenderObjectTags GetTag() const noexcept override { return RenderObjectTag::CmdEncoder; }

    virtual CommandBuffer* GetCommandBuffer() const noexcept = 0;

    virtual void BindRootSignature(RootSignature* rootSig) noexcept = 0;

    virtual void BindDescriptorSet(DescriptorSetIndex setIndex, DescriptorSet* set) noexcept = 0;

    virtual void PushConstants(BindingParameterId id, const void* data, uint32_t size) noexcept = 0;

    virtual void BindBindlessArray(DescriptorSetIndex set, BindlessArray* array) noexcept = 0;
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
};

class ComputeCommandEncoder : public CommandEncoder {
public:
    virtual ~ComputeCommandEncoder() noexcept = default;

    RenderObjectTags GetTag() const noexcept final { return RenderObjectTag::ComputeCmdEncoder; }

    virtual void BindComputePipelineState(ComputePipelineState* pso) noexcept = 0;

    virtual void Dispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) noexcept = 0;
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

    virtual AcquireResult AcquireNext(uint64_t timeoutMs = std::numeric_limits<uint64_t>::max()) noexcept = 0;

    virtual void Present(SwapChainSyncObject* waitToPresent) noexcept = 0;

    virtual Nullable<Texture*> GetCurrentBackBuffer() const noexcept = 0;

    virtual uint32_t GetCurrentBackBufferIndex() const noexcept = 0;

    virtual uint32_t GetBackBufferCount() const noexcept = 0;

    virtual SwapChainDescriptor GetDesc() const noexcept = 0;
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

    virtual void Unmap(uint64_t offset, uint64_t size) noexcept = 0;

    virtual BufferDescriptor GetDesc() const noexcept = 0;
};

class BufferView : public ResourceView {
public:
    virtual ~BufferView() noexcept = default;

    RenderObjectTags GetTag() const noexcept final { return RenderObjectTag::BufferView; }
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

class Shader : public RenderBase {
public:
    virtual ~Shader() noexcept = default;

    RenderObjectTags GetTag() const noexcept final { return RenderObjectTag::Shader; }

    virtual ShaderStages GetStages() const noexcept = 0;

    virtual Nullable<const ShaderReflectionDesc*> GetReflection() const noexcept = 0;
};

class RootSignature : public RenderBase, public IDebugName {
public:
    virtual ~RootSignature() noexcept = default;

    RenderObjectTags GetTag() const noexcept final { return RenderObjectTag::RootSignature; }

    virtual const BindingLayout& GetBindingLayout() const noexcept = 0;

    virtual uint32_t GetDescriptorSetCount() const noexcept = 0;

    virtual std::span<const BindingParameterLayout> GetDescriptorSetLayout(DescriptorSetIndex set) const noexcept = 0;

    virtual uint32_t GetBindlessSetCount() const noexcept = 0;

    virtual std::span<const BindlessSetLayout> GetBindlessSetLayouts() const noexcept = 0;

    virtual uint32_t GetStaticSamplerCount() const noexcept = 0;

    virtual std::span<const StaticSamplerLayout> GetStaticSamplerLayouts() const noexcept = 0;

    virtual std::span<const PushConstantRange> GetPushConstantRanges() const noexcept = 0;

    bool HasBindlessSet(DescriptorSetIndex set) const noexcept {
        return FindBindlessSet(set).HasValue();
    }

    Nullable<const BindlessSetLayout*> FindBindlessSet(DescriptorSetIndex set) const noexcept {
        for (const auto& bindlessSet : GetBindlessSetLayouts()) {
            if (bindlessSet.Set == set) {
                return &bindlessSet;
            }
        }
        return nullptr;
    }

    Nullable<const StaticSamplerLayout*> FindStaticSampler(BindingParameterId id) const noexcept {
        for (const auto& staticSampler : GetStaticSamplerLayouts()) {
            if (staticSampler.Id == id) {
                return &staticSampler;
            }
        }
        return nullptr;
    }

    Nullable<const StaticSamplerLayout*> FindStaticSampler(DescriptorSetIndex set, uint32_t binding) const noexcept {
        for (const auto& staticSampler : GetStaticSamplerLayouts()) {
            if (staticSampler.Set == set && staticSampler.Binding == binding) {
                return &staticSampler;
            }
        }
        return nullptr;
    }

    std::optional<BindingParameterId> FindParameterId(std::string_view name) const noexcept {
        return GetBindingLayout().FindParameterId(name);
    }

    Nullable<const BindingParameterLayout*> FindParameter(BindingParameterId id) const noexcept {
        return GetBindingLayout().FindParameter(id);
    }

    Nullable<const PushConstantRange*> FindPushConstantRange(BindingParameterId id) const noexcept {
        for (const auto& range : GetPushConstantRanges()) {
            if (range.Id == id) {
                return &range;
            }
        }
        return nullptr;
    }
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

class DescriptorSet : public RenderBase, public IDebugName {
public:
    virtual ~DescriptorSet() noexcept = default;

    RenderObjectTags GetTag() const noexcept final { return RenderObjectTag::DescriptorSet; }

    virtual RootSignature* GetRootSignature() const noexcept = 0;

    virtual DescriptorSetIndex GetSetIndex() const noexcept = 0;

    virtual bool WriteResource(BindingParameterId id, ResourceView* view, uint32_t arrayIndex = 0) noexcept = 0;

    virtual bool WriteSampler(BindingParameterId id, Sampler* sampler, uint32_t arrayIndex = 0) noexcept = 0;

    bool WriteResource(std::string_view name, ResourceView* view, uint32_t arrayIndex = 0) noexcept {
        auto idOpt = ResolveParameterId(name);
        if (!idOpt.has_value()) {
            return false;
        }
        return this->WriteResource(idOpt.value(), view, arrayIndex);
    }

    bool WriteSampler(std::string_view name, Sampler* sampler, uint32_t arrayIndex = 0) noexcept {
        auto idOpt = ResolveParameterId(name);
        if (!idOpt.has_value()) {
            return false;
        }
        return this->WriteSampler(idOpt.value(), sampler, arrayIndex);
    }

private:
    std::optional<BindingParameterId> ResolveParameterId(std::string_view name) const noexcept {
        return this->GetRootSignature()->FindParameterId(name);
    }
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

    virtual void SetBuffer(uint32_t slot, BufferView* bufView) noexcept = 0;

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
};

Nullable<shared_ptr<Device>> CreateDevice(const DeviceDescriptor& desc);

Nullable<unique_ptr<InstanceVulkan>> CreateVulkanInstance(const VulkanInstanceDescriptor& desc);

void DestroyVulkanInstance(unique_ptr<InstanceVulkan> instance) noexcept;

// --------------------------- Utility Functions ---------------------------
bool IsDepthStencilFormat(TextureFormat format) noexcept;
bool IsUintFormat(TextureFormat format) noexcept;
bool IsSintFormat(TextureFormat format) noexcept;
uint32_t GetVertexFormatSizeInBytes(VertexFormat format) noexcept;
uint32_t GetIndexFormatSizeInBytes(IndexFormat format) noexcept;
IndexFormat SizeInBytesToIndexFormat(uint32_t size) noexcept;
uint32_t GetTextureFormatBytesPerPixel(TextureFormat format) noexcept;
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

}  // namespace radray::render
