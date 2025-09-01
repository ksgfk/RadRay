#pragma once

#include <variant>
#include <optional>
#include <span>

#include <radray/types.h>
#include <radray/nullable.h>
#include <radray/enum_flags.h>
#include <radray/basic_math.h>

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
    std::span<Fence*> WaitFences;
    std::span<uint64_t> WaitFenceValues;
    std::span<Fence*> SignalFences;
    std::span<uint64_t> SignalFenceValues;
};

struct BarrierBufferDescriptor {
    Buffer* Target;
    BufferUses Before;
    BufferUses After;
    Nullable<CommandQueue> OtherQueue;
    bool IsFromOrToOtherQueue;  // true: from, false: to
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

struct ColorAttachment {
    TextureView* Target;
    LoadAction Load;
    StoreAction Store;
    ColorClearValue ClearValue;
};

struct DepthStencilAttachment {
    TextureView* Target;
    LoadAction DepthLoad;
    StoreAction DepthStore;
    LoadAction StencilLoad;
    StoreAction StencilStore;
    DepthStencilClearValue ClearValue;
};

struct RenderPassDescriptor {
    std::span<ColorAttachment> ColorAttachments;
    std::optional<DepthStencilAttachment> DepthStencilAttachment;
    std::string_view Name;
};

struct TextureDescriptor {
    TextureDimension Dim;
    uint32_t Width;
    uint32_t Height;
    uint32_t DepthOrArraySize;
    uint32_t MipLevels;
    uint32_t SampleCount;
    TextureFormat Format;
    TextureUses Usage;
    ResourceHints Hints;
    std::string_view Name;
};

struct TextureViewDescriptor {
    Texture* Target;
    TextureViewDimension Dim;
    TextureFormat Format;
    uint32_t BaseArrayLayer;
    std::optional<uint32_t> ArrayLayerCount;
    uint32_t BaseMipLevel;
    std::optional<uint32_t> MipLevelCount;
};

struct BufferDescriptor {
    uint64_t Size;
    MemoryType Memory;
    BufferUses Usage;
    ResourceHints Hints;
    std::string_view Name;
};

struct BufferRange {
    uint64_t Offset;
    uint64_t Size;
};

struct BufferViewDescriptor {
    Buffer* Target;
    BufferRange Range;
    TextureFormat Format;
    BufferUses Usage;
};

struct ShaderDescriptor {
    std::span<const byte> Source;
    ShaderBlobCategory Category;
};

struct RootSignatureConstant {
    uint32_t Slot;
    uint32_t Space;
    uint32_t Size;
    ShaderStages Stages;
};

struct RootSignatureBinding {
    uint32_t Slot;
    uint32_t Space;
    ResourceBindType Type;
    ShaderStages Stages;
};

struct RootSignatureSetElement {
    uint32_t Slot;
    uint32_t Space;
    ResourceBindType Type;
    uint32_t Count;
    ShaderStages Stages;
};

struct RootSignatureBindingSet {
    std::span<RootSignatureSetElement> Elements;
};

struct RootSignatureDescriptor {
    std::span<RootSignatureBinding> RootBindings;
    std::span<RootSignatureBindingSet> BindingSets;
    std::optional<RootSignatureConstant> Constant;
};

struct VertexElement {
    uint64_t Offset;
    std::string_view Semantic;
    uint32_t SemanticIndex;
    VertexFormat Format;
    uint32_t Location;
};

struct VertexInfo {
    uint64_t ArrayStride;
    VertexStepMode StepMode;
    std::span<VertexElement> Elements;
};

struct PrimitiveState {
    PrimitiveTopology Topology;
    FrontFace FaceClockwise;
    CullMode Cull;
    PolygonMode Poly;
    std::optional<IndexFormat> StripIndexFormat;
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
    DepthBiasState DepthBias;
    std::optional<StencilState> Stencil;
    bool DepthWriteEnable;
};

struct MultiSampleState {
    uint32_t Count;
    uint64_t Mask;
    bool AlphaToCoverageEnable;
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
    std::optional<BlendState> Blend;
    ColorWrites WriteMask;
};

struct ShaderEntry {
    Shader* Target;
    std::string_view EntryPoint;
};

struct GraphicsPipelineStateDescriptor {
    RootSignature* RootSig;
    std::optional<ShaderEntry> VS;
    std::optional<ShaderEntry> PS;
    std::span<VertexInfo> VertexLayouts;
    PrimitiveState Primitive;
    std::optional<DepthStencilState> DepthStencil;
    MultiSampleState MultiSample;
    std::span<ColorTargetState> ColorTargets;
};

struct VertexBufferView {
    Buffer* Target;
    uint64_t Offset;
    uint64_t Size;
};

struct IndexBufferView {
    Buffer* Target;
    uint32_t Offset;
    uint32_t Stride;
};

class Device : public enable_shared_from_this<Device>, public RenderBase {
public:
    virtual ~Device() noexcept = default;

    RenderObjectTags GetTag() const noexcept final { return RenderObjectTag::Device; }

    virtual Backend GetBackend() noexcept = 0;

    virtual Nullable<CommandQueue> GetCommandQueue(QueueType type, uint32_t slot = 0) noexcept = 0;

    virtual Nullable<shared_ptr<CommandBuffer>> CreateCommandBuffer(CommandQueue* queue) noexcept = 0;

    virtual Nullable<shared_ptr<Fence>> CreateFence(uint64_t initValue) noexcept = 0;

    virtual Nullable<shared_ptr<SwapChain>> CreateSwapChain(const SwapChainDescriptor& desc) noexcept = 0;

    virtual Nullable<shared_ptr<Buffer>> CreateBuffer(const BufferDescriptor& desc) noexcept = 0;

    virtual Nullable<shared_ptr<BufferView>> CreateBufferView(const BufferViewDescriptor& desc) noexcept = 0;

    virtual Nullable<shared_ptr<Texture>> CreateTexture(const TextureDescriptor& desc) noexcept = 0;

    virtual Nullable<shared_ptr<TextureView>> CreateTextureView(const TextureViewDescriptor& desc) noexcept = 0;

    virtual Nullable<shared_ptr<Shader>> CreateShader(const ShaderDescriptor& desc) noexcept = 0;

    virtual Nullable<shared_ptr<RootSignature>> CreateRootSignature(const RootSignatureDescriptor& desc) noexcept = 0;

    virtual Nullable<shared_ptr<GraphicsPipelineState>> CreateGraphicsPipelineState(const GraphicsPipelineStateDescriptor& desc) noexcept = 0;
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

    virtual void ResourceBarrier(std::span<BarrierBufferDescriptor> buffers, std::span<BarrierTextureDescriptor> textures) noexcept = 0;

    virtual Nullable<unique_ptr<CommandEncoder>> BeginRenderPass(const RenderPassDescriptor& desc) noexcept = 0;

    virtual void EndRenderPass(unique_ptr<CommandEncoder> encoder) noexcept = 0;

    virtual void CopyBufferToBuffer(Buffer* dst, uint64_t dstOffset, Buffer* src, uint64_t srcOffset, uint64_t size) noexcept = 0;
};

class CommandEncoder : public RenderBase {
public:
    virtual ~CommandEncoder() noexcept = default;

    RenderObjectTags GetTag() const noexcept final { return RenderObjectTag::CmdEncoder; }

    virtual void SetViewport(Viewport vp) noexcept = 0;

    virtual void SetScissor(Rect rect) noexcept = 0;

    virtual void BindVertexBuffer(std::span<VertexBufferView> vbv) noexcept = 0;

    virtual void BindIndexBuffer(IndexBufferView ibv) noexcept = 0;

    virtual void BindRootSignature(RootSignature* rootSig) noexcept = 0;

    virtual void BindGraphicsPipelineState(GraphicsPipelineState* pso) noexcept = 0;

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

    virtual Nullable<Texture> AcquireNext() noexcept = 0;

    virtual void Present() noexcept = 0;

    virtual Nullable<Texture> GetCurrentBackBuffer() const noexcept = 0;

    virtual uint32_t GetCurrentBackBufferIndex() const noexcept = 0;

    virtual uint32_t GetBackBufferCount() const noexcept = 0;
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

    virtual void CopyFromHost(std::span<byte> data, uint64_t offset) noexcept = 0;
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

class Shader : public Resource {
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

bool GlobalInitGraphics(std::span<BackendInitDescriptor> descs);

void GlobalTerminateGraphics();

Nullable<shared_ptr<Device>> CreateDevice(const DeviceDescriptor& desc);

bool IsDepthStencilFormat(TextureFormat format) noexcept;

uint32_t GetVertexFormatSize(VertexFormat format) noexcept;

PrimitiveState DefaultPrimitiveState() noexcept;

DepthStencilState DefaultDepthStencilState() noexcept;

StencilState DefaultStencilState() noexcept;

MultiSampleState DefaultMultiSampleState() noexcept;

ColorTargetState DefaultColorTargetState(TextureFormat format) noexcept;

BlendState DefaultBlendState() noexcept;

std::string_view format_as(Backend v) noexcept;
std::string_view format_as(TextureFormat v) noexcept;
std::string_view format_as(QueueType v) noexcept;
std::string_view format_as(ShaderBlobCategory v) noexcept;
std::string_view format_as(VertexFormat v) noexcept;
std::string_view format_as(PolygonMode v) noexcept;
std::string_view format_as(TextureViewDimension v) noexcept;

}  // namespace radray::render
