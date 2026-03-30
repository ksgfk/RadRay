#pragma once

#include <numeric>
#include <optional>
#include <variant>

#include <radray/types.h>
#include <radray/enum_flags.h>
#include <radray/render/common.h>
#include <radray/runtime/gpu_system.h>

namespace radray {

class RenderGraph;
class RDGNode;
class RDGEdge;

enum class RDGNodeTag : uint32_t {
    UNKNOWN = 0x0,
    Resource = 0x1,
    Buffer = Resource | (Resource << 1),
    Texture = Resource | (Resource << 2),
    Pass = Resource << 3,
    GraphicsPass = Pass | (Pass << 1),
    ComputePass = Pass | (Pass << 2),
    CopyPass = Pass | (Pass << 3),
};

enum class RDGExecutionStage : uint32_t {
    NONE = 0x0,
    VertexInput = 0x1,
    VertexShader = VertexInput << 1,
    PixelShader = VertexShader << 1,
    DepthStencil = PixelShader << 1,
    ColorOutput = DepthStencil << 1,
    Indirect = ColorOutput << 1,
    ComputeShader = Indirect << 1,
    Copy = ComputeShader << 1,
    Host = Copy << 1,
    Present = Host << 1
};

enum class RDGMemoryAccess : uint32_t {
    NONE = 0x0,
    VertexRead = 0x1,
    IndexRead = VertexRead << 1,
    ConstantRead = IndexRead << 1,
    ShaderRead = ConstantRead << 1,
    ShaderWrite = ShaderRead << 1,
    ColorAttachmentRead = ShaderWrite << 1,
    ColorAttachmentWrite = ColorAttachmentRead << 1,
    DepthStencilRead = ColorAttachmentWrite << 1,
    DepthStencilWrite = DepthStencilRead << 1,
    TransferRead = DepthStencilWrite << 1,
    TransferWrite = TransferRead << 1,
    HostRead = TransferWrite << 1,
    HostWrite = HostRead << 1,
    IndirectRead = HostWrite << 1,
};

enum class RDGTextureLayout {
    UNKNOWN = 0,
    Undefined,
    General,
    ShaderReadOnly,
    ColorAttachment,
    DepthStencilReadOnly,
    DepthStencilAttachment,
    TransferSource,
    TransferDestination,
    Present
};

enum class RDGResourceOwnership {
    UNKNOWN,
    External,
    Internal,
    Transient
};

template <>
struct is_flags<RDGNodeTag> : public std::true_type {};
template <>
struct is_flags<RDGExecutionStage> : public std::true_type {};
template <>
struct is_flags<RDGMemoryAccess> : public std::true_type {};

using RDGNodeTags = EnumFlags<RDGNodeTag>;
using RDGExecutionStages = EnumFlags<RDGExecutionStage>;
using RDGMemoryAccesses = EnumFlags<RDGMemoryAccess>;

struct RDGNodeHandle {
    uint64_t Id{std::numeric_limits<uint64_t>::max()};

    bool IsValid() const noexcept {
        return Id != std::numeric_limits<uint64_t>::max();
    }
};

struct RDGResourceHandle : public RDGNodeHandle {};
struct RDGBufferHandle : public RDGResourceHandle {};
struct RDGTextureHandle : public RDGResourceHandle {};
struct RDGPassHandle : public RDGNodeHandle {};

// --------------------------- Internal ---------------------------

struct RDGBufferState {
    RDGExecutionStage Stage{RDGExecutionStage::NONE};
    RDGMemoryAccess Access{RDGMemoryAccess::NONE};
    render::BufferRange Range{};
};

struct RDGTextureState {
    RDGExecutionStage Stage{RDGExecutionStage::NONE};
    RDGMemoryAccess Access{RDGMemoryAccess::NONE};
    RDGTextureLayout Layout{RDGTextureLayout::UNKNOWN};
    render::SubresourceRange Range{};
};

struct RDGColorAttachmentInfo {
    uint32_t Slot{0};
    RDGTextureHandle Texture{};
    render::SubresourceRange Range{};
    render::LoadAction Load{render::LoadAction::DontCare};
    render::StoreAction Store{render::StoreAction::Store};
    std::optional<render::ColorClearValue> ClearValue{};
};

struct RDGDepthStencilAttachmentInfo {
    RDGTextureHandle Texture{};
    render::SubresourceRange Range{};
    render::LoadAction DepthLoad{render::LoadAction::DontCare};
    render::StoreAction DepthStore{render::StoreAction::Store};
    render::LoadAction StencilLoad{render::LoadAction::DontCare};
    render::StoreAction StencilStore{render::StoreAction::Store};
    std::optional<render::DepthStencilClearValue> ClearValue{};

    bool HasWriteAccess() const noexcept {
        return DepthLoad == render::LoadAction::Clear || StencilLoad == render::LoadAction::Clear || DepthStore == render::StoreAction::Store || StencilStore == render::StoreAction::Store;
    }
};

struct RDGCopyBufferToBufferInfo {
    RDGBufferHandle Dst{};
    uint64_t DstOffset{0};
    RDGBufferHandle Src{};
    uint64_t SrcOffset{0};
    uint64_t Size{0};
};

struct RDGCopyBufferToTextureInfo {
    RDGTextureHandle Dst{};
    render::SubresourceRange DstRange{};
    RDGBufferHandle Src{};
    uint64_t SrcOffset{0};
};

struct RDGCopyTextureToBufferInfo {
    RDGBufferHandle Dst{};
    uint64_t DstOffset{0};
    RDGTextureHandle Src{};
    render::SubresourceRange SrcRange{};
};

using RDGCopyPassOp = std::variant<
    RDGCopyBufferToBufferInfo,
    RDGCopyBufferToTextureInfo,
    RDGCopyTextureToBufferInfo>;

struct RDGPassDependency {
    RDGPassHandle Before{};
    RDGPassHandle After{};
    RDGResourceHandle Resource{};
};

struct RDGCompiledBufferBarrier {
    RDGBufferHandle Buffer{};
    RDGBufferState Before{};
    RDGBufferState After{};
};

struct RDGCompiledTextureBarrier {
    RDGTextureHandle Texture{};
    RDGTextureState Before{};
    RDGTextureState After{};
};

using RDGCompiledBarrier = std::variant<
    RDGCompiledBufferBarrier,
    RDGCompiledTextureBarrier>;

struct RDGCompiledPass {
    RDGPassHandle Pass{};
    vector<RDGPassHandle> Predecessors;
    vector<RDGCompiledBarrier> BarriersBefore;
    vector<RDGCompiledBarrier> BarriersAfter;
};

struct RDGCompiledResourceLifetime {
    RDGResourceHandle Resource{};
    std::optional<uint32_t> FirstPassIndex{};
    std::optional<uint32_t> LastPassIndex{};
};

struct RDGCompileResult {
    vector<RDGPassHandle> PassOrder;
    vector<RDGPassDependency> Dependencies;
    vector<RDGCompiledPass> Passes;
    vector<RDGCompiledResourceLifetime> Lifetimes;
};

// ----------------------------------------------------------------

class RDGNode {
public:
    RDGNode(uint64_t id, std::string_view name) noexcept
        : _name(name),
          _id(id) {}
    virtual ~RDGNode() noexcept = default;

    virtual RDGNodeTags GetTag() const noexcept = 0;

public:
    string _name;
    uint64_t _id{std::numeric_limits<uint64_t>::max()};
    vector<RDGEdge*> _inEdges;
    vector<RDGEdge*> _outEdges;
};

class RDGResourceNode : public RDGNode {
public:
    RDGResourceNode(uint64_t id, std::string_view name, RDGResourceOwnership ownership) noexcept
        : RDGNode(id, name),
          _ownership(ownership) {}
    virtual ~RDGResourceNode() noexcept = default;

    RDGNodeTags GetTag() const noexcept override { return RDGNodeTag::Resource; }

public:
    RDGResourceOwnership _ownership{RDGResourceOwnership::UNKNOWN};
    uint32_t _exportCount{0};
};

class RDGBufferNode final : public RDGResourceNode {
public:
    using RDGResourceNode::RDGResourceNode;
    virtual ~RDGBufferNode() noexcept = default;

    RDGNodeTags GetTag() const noexcept override { return RDGNodeTag::Buffer; }

public:
    uint64_t _size{0};
    GpuBufferHandle _backingHandle{};
    std::optional<RDGBufferState> _importedState{};
    std::optional<RDGBufferState> _exportedState{};
};

class RDGTextureNode final : public RDGResourceNode {
public:
    using RDGResourceNode::RDGResourceNode;
    virtual ~RDGTextureNode() noexcept = default;

    RDGNodeTags GetTag() const noexcept override { return RDGNodeTag::Texture; }

public:
    GpuTextureHandle _backingHandle{};
    render::TextureDimension _dim{render::TextureDimension::UNKNOWN};
    uint32_t _width{0};
    uint32_t _height{0};
    uint32_t _depthOrArraySize{0};
    uint32_t _mipLevels{0};
    uint32_t _sampleCount{0};
    render::TextureFormat _format{render::TextureFormat::UNKNOWN};
    std::optional<RDGTextureState> _importedState{};
    std::optional<RDGTextureState> _exportedState{};
};

class RDGPassNode : public RDGNode {
public:
    using RDGNode::RDGNode;
    virtual ~RDGPassNode() noexcept = default;

    RDGNodeTags GetTag() const noexcept override { return RDGNodeTag::Pass; }
};

class RDGGraphicsPassNode final : public RDGPassNode {
public:
    using RDGPassNode::RDGPassNode;
    virtual ~RDGGraphicsPassNode() noexcept = default;

    RDGNodeTags GetTag() const noexcept override { return RDGNodeTag::GraphicsPass; }

public:
    vector<RDGColorAttachmentInfo> _colorAttachments;
    std::optional<RDGDepthStencilAttachmentInfo> _depthStencilAttachment{};
};

class RDGComputePassNode final : public RDGPassNode {
public:
    using RDGPassNode::RDGPassNode;
    virtual ~RDGComputePassNode() noexcept = default;

    RDGNodeTags GetTag() const noexcept override { return RDGNodeTag::ComputePass; }
};

class RDGCopyPassNode final : public RDGPassNode {
public:
    using RDGPassNode::RDGPassNode;
    virtual ~RDGCopyPassNode() noexcept = default;

    RDGNodeTags GetTag() const noexcept override { return RDGNodeTag::CopyPass; }

public:
    vector<RDGCopyPassOp> _ops;
};

class RDGEdge {
public:
    RDGEdge(
        RDGNode* from,
        RDGNode* to,
        RDGExecutionStage stage,
        RDGMemoryAccess access) noexcept
        : _from(from),
          _to(to),
          _stage(stage),
          _access(access) {}

public:
    RDGNode* _from{nullptr};
    RDGNode* _to{nullptr};
    RDGExecutionStage _stage{RDGExecutionStage::NONE};
    RDGMemoryAccess _access{RDGMemoryAccess::NONE};
    render::BufferRange _bufferRange;
    RDGTextureLayout _textureLayout{RDGTextureLayout::UNKNOWN};
    render::SubresourceRange _textureRange;
};

class RDGRasterPassBuilder {
public:
    explicit RDGRasterPassBuilder(RenderGraph* graph) noexcept : _graph(graph) {}

    RDGPassHandle Build();

    RDGRasterPassBuilder& UseColorAttachment(
        uint32_t slot,
        RDGTextureHandle texture,
        render::SubresourceRange range,
        render::LoadAction load,
        render::StoreAction store,
        std::optional<render::ColorClearValue> clearValue);
    RDGRasterPassBuilder& UseDepthStencilAttachment(
        RDGTextureHandle texture,
        render::SubresourceRange range,
        render::LoadAction depthLoad, render::StoreAction depthStore,
        render::LoadAction stencilLoad, render::StoreAction stencilStore,
        std::optional<render::DepthStencilClearValue> clearValue);

    RDGRasterPassBuilder& UseVertexBuffer(RDGBufferHandle buffer, render::BufferRange range);
    RDGRasterPassBuilder& UseIndexBuffer(RDGBufferHandle buffer, render::BufferRange range);
    RDGRasterPassBuilder& UseIndirectBuffer(RDGBufferHandle buffer, render::BufferRange range);
    RDGRasterPassBuilder& UseCBuffer(RDGBufferHandle buffer, render::ShaderStages stages, render::BufferRange range);
    RDGRasterPassBuilder& UseBuffer(RDGBufferHandle buffer, render::ShaderStages stages, render::BufferRange range);
    RDGRasterPassBuilder& UseRWBuffer(RDGBufferHandle buffer, render::ShaderStages stages, render::BufferRange range);

    RDGRasterPassBuilder& UseTexture(RDGTextureHandle texture, render::ShaderStages stages, render::SubresourceRange range);
    RDGRasterPassBuilder& UseRWTexture(RDGTextureHandle texture, render::ShaderStages stages, render::SubresourceRange range);

public:
    RDGPassHandle _EnsurePass();
    void _ValidateShaderStages(render::ShaderStages stages) const;
    void _LinkBufferStages(RDGBufferHandle buffer, render::ShaderStages stages, RDGMemoryAccess access, render::BufferRange range);
    void _LinkTextureStages(RDGTextureHandle texture, render::ShaderStages stages, RDGMemoryAccess access, RDGTextureLayout layout, render::SubresourceRange range);

    RenderGraph* _graph{nullptr};
    RDGPassHandle _pass{};
};

class RDGComputePassBuilder {
public:
    explicit RDGComputePassBuilder(RenderGraph* graph) noexcept : _graph(graph) {}

    RDGPassHandle Build();

    RDGComputePassBuilder& UseCBuffer(RDGBufferHandle buffer, render::BufferRange range);
    RDGComputePassBuilder& UseBuffer(RDGBufferHandle buffer, render::BufferRange range);
    RDGComputePassBuilder& UseRWBuffer(RDGBufferHandle buffer, render::BufferRange range);

    RDGComputePassBuilder& UseTexture(RDGTextureHandle texture, render::SubresourceRange range);
    RDGComputePassBuilder& UseRWTexture(RDGTextureHandle texture, render::SubresourceRange range);

public:
    RDGPassHandle _EnsurePass();

    RenderGraph* _graph{nullptr};
    RDGPassHandle _pass{};
};

class RDGCopyPassBuilder {
public:
    explicit RDGCopyPassBuilder(RenderGraph* graph) noexcept : _graph(graph) {}

    RDGPassHandle Build();

    RDGCopyPassBuilder& CopyBufferToBuffer(RDGBufferHandle dst, uint64_t dstOffset, RDGBufferHandle src, uint64_t srcOffset, uint64_t size);
    RDGCopyPassBuilder& CopyBufferToTexture(RDGTextureHandle dst, render::SubresourceRange dstRange, RDGBufferHandle src, uint64_t srcOffset);
    RDGCopyPassBuilder& CopyTextureToBuffer(RDGBufferHandle dst, uint64_t dstOffset, RDGTextureHandle src, render::SubresourceRange srcRange);

public:
    RDGPassHandle _EnsurePass();

    RenderGraph* _graph{nullptr};
    RDGPassHandle _pass{};
};

class RenderGraph {
public:
    // ---------------- Core ----------------
    // RDG 创建的资源
    RDGBufferHandle AddBuffer(uint64_t size, std::string_view name);
    RDGTextureHandle AddTexture(
        render::TextureDimension dim,
        uint32_t width, uint32_t height,
        uint32_t depthOrArraySize, uint32_t mipLevels,
        uint32_t sampleCount,
        render::TextureFormat format,
        std::string_view name);
    // 导入外部资源
    RDGBufferHandle ImportBuffer(GpuBufferHandle buffer, RDGExecutionStage stage, RDGMemoryAccess access, render::BufferRange bufferRange, std::string_view name);
    RDGTextureHandle ImportTexture(GpuTextureHandle texture, RDGExecutionStage stage, RDGMemoryAccess access, RDGTextureLayout layout, render::SubresourceRange textureRange, std::string_view name);
    // 最终要导出的资源
    void ExportBuffer(RDGBufferHandle node, RDGExecutionStage stage, RDGMemoryAccess access, render::BufferRange bufferRange);
    void ExportTexture(RDGTextureHandle node, RDGExecutionStage stage, RDGMemoryAccess access, RDGTextureLayout layout, render::SubresourceRange textureRange);
    // TODO: pass 应该还有其他数据
    RDGPassHandle AddPass(std::string_view name, RDGNodeTag tag = RDGNodeTag::GraphicsPass);
    // 图连接
    void Link(RDGNodeHandle from, RDGNodeHandle to, RDGExecutionStage stage, RDGMemoryAccess access, render::BufferRange bufferRange);
    void Link(RDGNodeHandle from, RDGNodeHandle to, RDGExecutionStage stage, RDGMemoryAccess access, RDGTextureLayout layout, render::SubresourceRange textureRange);
    RDGCompileResult Compile() const;
    std::pair<bool, string> Validate() const;

    // ---------------- helper ----------------
    string ExportGraphviz() const;
    string ExportCompiledGraphviz(const RDGCompileResult& compiled) const;
    string ExportExecutionGraphviz(const RDGCompileResult& compiled) const;

public:
    RDGEdge* _CreateEdge(RDGNode* from, RDGNode* to, RDGExecutionStage stage, RDGMemoryAccess access);

    GpuRuntime* _gpu{nullptr};
    vector<unique_ptr<RDGNode>> _nodes;
    vector<unique_ptr<RDGEdge>> _edges;
};

std::string_view format_as(RDGNodeTag v) noexcept;
std::string_view format_as(RDGExecutionStage v) noexcept;
std::string_view format_as(RDGMemoryAccess v) noexcept;
std::string_view format_as(RDGTextureLayout v) noexcept;
std::string_view format_as(RDGResourceOwnership v) noexcept;

}  // namespace radray
