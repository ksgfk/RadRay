#pragma once

#include <variant>
#include <optional>
#include <span>

#include <radray/types.h>
#include <radray/nullable.h>
#include <radray/enum_flags.h>
#include <radray/basic_math.h>

namespace radray::render {

enum class RenderBackend {
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
    Uninitialized = 0x1,
    Present = Uninitialized << 1,
    CopySource = Present << 1,
    CopyDestination = CopySource << 1,
    Resource = CopyDestination << 1,
    RenderTarget = Resource << 1,
    DepthStencilRead = RenderTarget << 1,
    DepthStencilWrite = DepthStencilRead << 1,
    UnorderedAccess = DepthStencilWrite << 1,
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

enum class MemoryType {
    Device,
    Upload,
    ReadBack,
};

enum class ResourceBindType {
    UNKNOWN,
    CBuffer,
    Buffer,
    Texture,
    Sampler,
    RWBuffer,
    RWTexture
};

enum class RenderObjectTag : uint32_t {
    UNKNOWN = 0x0,
    Device = 0x1,
    CmdQueue = Device << 1,
    CmdBuffer = CmdQueue << 1,
    CmdEncoder = CmdBuffer << 1,
    Fence = CmdEncoder << 1,
    Shader = Fence << 1,
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
    Sampler = DescriptorSet << 1,

    VkInstance = Sampler << 1
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
class CommandEncoder;
class Fence;
class SwapChain;
class Resource;
class ResourceView;
class Buffer;
class BufferView;
class Texture;
class TextureView;
class Shader;
class RootSignature;
class PipelineState;
class GraphicsPipelineState;
class DescriptorSet;
class Sampler;
class InstanceVulkan;

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

class VulkanInstanceDescriptor {
public:
    std::string_view AppName{};
    uint32_t AppVersion{0};
    std::string_view EngineName{};
    uint32_t EngineVersion{0};
    bool IsEnableDebugLayer{false};
    bool IsEnableGpuBasedValid{false};
};

class D3D12DeviceDescriptor {
public:
    std::optional<uint32_t> AdapterIndex{};
    bool IsEnableDebugLayer{false};
    bool IsEnableGpuBasedValid{false};
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
    bool EnableSync{false};
};

struct SamplerDescriptor {
    AddressMode AddressS{};
    AddressMode AddressT{};
    AddressMode AddressR{};
    FilterMode MigFilter{};
    FilterMode MagFilter{};
    FilterMode MipmapFilter{};
    float LodMin{0.0f};
    float LodMax{0.0f};
    std::optional<CompareFunction> Compare{};
    uint32_t AnisotropyClamp{0};

    friend bool operator==(const SamplerDescriptor& lhs, const SamplerDescriptor& rhs) noexcept;
    friend bool operator!=(const SamplerDescriptor& lhs, const SamplerDescriptor& rhs) noexcept;
};

struct CommandQueueSubmitDescriptor {
    std::span<CommandBuffer* const> CmdBuffers{};
    std::span<Fence* const> WaitFences{};
    std::span<const uint64_t> WaitFenceValues{};
    std::span<Fence* const> SignalFences{};
    std::span<const uint64_t> SignalFenceValues{};
};

struct BarrierBufferDescriptor {
    Buffer* Target{nullptr};
    BufferUses Before{BufferUse::UNKNOWN};
    BufferUses After{BufferUse::UNKNOWN};
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
    TextureUses Before{TextureUse::UNKNOWN};
    TextureUses After{TextureUse::UNKNOWN};
    Nullable<CommandQueue*> OtherQueue{nullptr};
    bool IsFromOrToOtherQueue{false};
    bool IsSubresourceBarrier{false};
    SubresourceRange Range{};
};

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
    TextureUses Usage{TextureUse::UNKNOWN};
    ResourceHints Hints{ResourceHint::None};
    std::string_view Name{};
};

struct TextureViewDescriptor {
    Texture* Target{nullptr};
    TextureViewDimension Dim{TextureViewDimension::UNKNOWN};
    TextureFormat Format{TextureFormat::UNKNOWN};
    SubresourceRange Range{};
    TextureUses Usage{TextureUse::UNKNOWN};
};

struct BufferDescriptor {
    uint64_t Size{0};
    MemoryType Memory{};
    BufferUses Usage{BufferUse::UNKNOWN};
    ResourceHints Hints{};
    std::string_view Name{};
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
    BufferUses Usage{BufferUse::UNKNOWN};
};

struct ShaderDescriptor {
    std::span<const byte> Source{};
    ShaderBlobCategory Category{};
};

struct RootSignatureConstant {
    uint32_t Slot{0};
    uint32_t Space{0};
    uint32_t Size{0};
    ShaderStages Stages{ShaderStage::UNKNOWN};
};

struct RootSignatureRootDescriptor {
    uint32_t Slot{0};
    uint32_t Space{0};
    ResourceBindType Type{ResourceBindType::UNKNOWN};
    ShaderStages Stages{ShaderStage::UNKNOWN};
};

struct RootSignatureSetElement {
    uint32_t Slot{0};
    uint32_t Space{0};
    ResourceBindType Type{ResourceBindType::UNKNOWN};
    uint32_t Count{0};
    ShaderStages Stages{ShaderStage::UNKNOWN};
    std::span<const SamplerDescriptor> StaticSamplers{};
};

struct RootSignatureDescriptorSet {
    std::span<const RootSignatureSetElement> Elements{};
};

struct RootSignatureDescriptor {
    std::span<const RootSignatureRootDescriptor> RootDescriptors{};
    std::span<const RootSignatureDescriptorSet> DescriptorSets{};
    std::optional<RootSignatureConstant> Constant{};
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

    static MultiSampleState Default() noexcept {
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

    static BlendState Default() noexcept {
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

    static ColorTargetState Default(TextureFormat format) noexcept {
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
    uint32_t CBufferAlignment{0};
    uint32_t UploadTextureAlignment{0};
    uint32_t UploadTextureRowAlignment{0};
    uint32_t MapAlignment{0};
};

class Device : public enable_shared_from_this<Device>, public RenderBase {
public:
    virtual ~Device() noexcept = default;

    RenderObjectTags GetTag() const noexcept final { return RenderObjectTag::Device; }

    virtual RenderBackend GetBackend() noexcept = 0;

    virtual DeviceDetail GetDetail() const noexcept = 0;

    virtual Nullable<CommandQueue*> GetCommandQueue(QueueType type, uint32_t slot = 0) noexcept = 0;

    virtual Nullable<unique_ptr<CommandBuffer>> CreateCommandBuffer(CommandQueue* queue) noexcept = 0;

    virtual Nullable<unique_ptr<Fence>> CreateFence(uint64_t initValue) noexcept = 0;

    virtual Nullable<unique_ptr<SwapChain>> CreateSwapChain(const SwapChainDescriptor& desc) noexcept = 0;

    virtual Nullable<unique_ptr<Buffer>> CreateBuffer(const BufferDescriptor& desc) noexcept = 0;

    virtual Nullable<unique_ptr<BufferView>> CreateBufferView(const BufferViewDescriptor& desc) noexcept = 0;

    virtual Nullable<unique_ptr<Texture>> CreateTexture(const TextureDescriptor& desc) noexcept = 0;

    virtual Nullable<unique_ptr<TextureView>> CreateTextureView(const TextureViewDescriptor& desc) noexcept = 0;

    virtual Nullable<unique_ptr<Shader>> CreateShader(const ShaderDescriptor& desc) noexcept = 0;

    virtual Nullable<unique_ptr<RootSignature>> CreateRootSignature(const RootSignatureDescriptor& desc) noexcept = 0;

    virtual Nullable<unique_ptr<GraphicsPipelineState>> CreateGraphicsPipelineState(const GraphicsPipelineStateDescriptor& desc) noexcept = 0;

    virtual Nullable<unique_ptr<DescriptorSet>> CreateDescriptorSet(RootSignature* rootSig, uint32_t index) noexcept = 0;

    virtual Nullable<unique_ptr<Sampler>> CreateSampler(const SamplerDescriptor& desc) noexcept = 0;
};

class CommandQueue : public RenderBase {
public:
    virtual ~CommandQueue() noexcept = default;

    RenderObjectTags GetTag() const noexcept final { return RenderObjectTag::CmdQueue; }

    virtual void Submit(const CommandQueueSubmitDescriptor& desc) noexcept = 0;

    virtual void Wait() noexcept = 0;
};

class CommandBuffer : public RenderBase {
public:
    virtual ~CommandBuffer() noexcept = default;

    RenderObjectTags GetTag() const noexcept final { return RenderObjectTag::CmdBuffer; }

    virtual void Begin() noexcept = 0;

    virtual void End() noexcept = 0;

    virtual void ResourceBarrier(std::span<const BarrierBufferDescriptor> buffers, std::span<const BarrierTextureDescriptor> textures) noexcept = 0;

    virtual Nullable<unique_ptr<CommandEncoder>> BeginRenderPass(const RenderPassDescriptor& desc) noexcept = 0;

    virtual void EndRenderPass(unique_ptr<CommandEncoder> encoder) noexcept = 0;

    virtual void CopyBufferToBuffer(Buffer* dst, uint64_t dstOffset, Buffer* src, uint64_t srcOffset, uint64_t size) noexcept = 0;

    virtual void CopyBufferToTexture(Texture* dst, SubresourceRange dstRange, Buffer* src, uint64_t srcOffset) noexcept = 0;
};

class CommandEncoder : public RenderBase {
public:
    virtual ~CommandEncoder() noexcept = default;

    RenderObjectTags GetTag() const noexcept final { return RenderObjectTag::CmdEncoder; }

    virtual void SetViewport(Viewport vp) noexcept = 0;

    virtual void SetScissor(Rect rect) noexcept = 0;

    virtual void BindVertexBuffer(std::span<const VertexBufferView> vbv) noexcept = 0;

    virtual void BindIndexBuffer(IndexBufferView ibv) noexcept = 0;

    virtual void BindRootSignature(RootSignature* rootSig) noexcept = 0;

    virtual void BindGraphicsPipelineState(GraphicsPipelineState* pso) noexcept = 0;

    virtual void PushConstant(const void* data, size_t length) noexcept = 0;

    virtual void BindRootDescriptor(uint32_t slot, ResourceView* view) noexcept = 0;

    virtual void BindDescriptorSet(uint32_t slot, DescriptorSet* set) noexcept = 0;

    virtual void Draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance) noexcept = 0;

    virtual void DrawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance) noexcept = 0;
};

class Fence : public RenderBase {
public:
    virtual ~Fence() noexcept = default;

    RenderObjectTags GetTag() const noexcept final { return RenderObjectTag::Fence; }

    virtual uint64_t GetCompletedValue() const noexcept = 0;
};

class SwapChain : public RenderBase {
public:
    virtual ~SwapChain() noexcept = default;

    RenderObjectTags GetTag() const noexcept final { return RenderObjectTag::SwapChain; }

    virtual Nullable<Texture*> AcquireNext() noexcept = 0;

    virtual void Present() noexcept = 0;

    virtual Nullable<Texture*> GetCurrentBackBuffer() const noexcept = 0;

    virtual uint32_t GetCurrentBackBufferIndex() const noexcept = 0;

    virtual uint32_t GetBackBufferCount() const noexcept = 0;

    virtual SwapChainDescriptor GetDesc() const noexcept = 0;
};

class Resource : public RenderBase {
public:
    virtual ~Resource() noexcept = default;
};

class ResourceView : public RenderBase {
public:
    virtual ~ResourceView() noexcept = default;
};

class Buffer : public Resource {
public:
    virtual ~Buffer() noexcept = default;

    RenderObjectTags GetTag() const noexcept final { return RenderObjectTag::Buffer; }

    virtual void* Map(uint64_t offset, uint64_t size) noexcept = 0;

    virtual void Unmap(uint64_t offset, uint64_t size) noexcept = 0;
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
};

class TextureView : public ResourceView {
public:
    virtual ~TextureView() noexcept = default;

    RenderObjectTags GetTag() const noexcept final { return RenderObjectTag::TextureView; }
};

class Shader : public RenderBase {
public:
    virtual ~Shader() noexcept = default;

    RenderObjectTags GetTag() const noexcept final { return RenderObjectTag::Shader; }
};

class RootSignature : public RenderBase {
public:
    virtual ~RootSignature() noexcept = default;

    RenderObjectTags GetTag() const noexcept final { return RenderObjectTag::RootSignature; }
};

class PipelineState : public RenderBase {
public:
    virtual ~PipelineState() noexcept = default;
};

class GraphicsPipelineState : public PipelineState {
public:
    virtual ~GraphicsPipelineState() noexcept = default;

    RenderObjectTags GetTag() const noexcept final { return RenderObjectTag::GraphicsPipelineState; }
};

class DescriptorSet : public RenderBase {
public:
    virtual ~DescriptorSet() noexcept = default;

    RenderObjectTags GetTag() const noexcept final { return RenderObjectTag::DescriptorSet; }

    virtual void SetResource(uint32_t slot, uint32_t index, ResourceView* view) noexcept = 0;
};

class Sampler : public RenderBase {
public:
    virtual ~Sampler() noexcept = default;

    RenderObjectTags GetTag() const noexcept final { return RenderObjectTag::Sampler; }
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
uint32_t GetVertexFormatSizeInBytes(VertexFormat format) noexcept;
uint32_t GetIndexFormatSizeInBytes(IndexFormat format) noexcept;
IndexFormat SizeInBytesToIndexFormat(uint32_t size) noexcept;
// -------------------------------------------------------------------------

std::string_view format_as(RenderBackend v) noexcept;
std::string_view format_as(TextureFormat v) noexcept;
std::string_view format_as(QueueType v) noexcept;
std::string_view format_as(ShaderBlobCategory v) noexcept;
std::string_view format_as(VertexFormat v) noexcept;
std::string_view format_as(PolygonMode v) noexcept;
std::string_view format_as(TextureViewDimension v) noexcept;
std::string_view format_as(ResourceBindType v) noexcept;
std::string_view format_as(RenderObjectTag v) noexcept;

}  // namespace radray::render
