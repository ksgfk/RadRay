#pragma once

#include <numeric>
#include <optional>
#include <type_traits>
#include <variant>

#include <radray/types.h>
#include <radray/enum_flags.h>
#include <radray/render/common.h>
#include <radray/runtime/gpu_system.h>

namespace radray {

struct RDGNodeHandle {
    uint64_t Id{std::numeric_limits<uint64_t>::max()};

    constexpr bool IsValid() const noexcept { return Id != std::numeric_limits<uint64_t>::max(); }

    constexpr auto operator<=>(const RDGNodeHandle&) const noexcept = default;

    constexpr static RDGNodeHandle Invalid() { return {std::numeric_limits<uint64_t>::max()}; }
};

struct RDGResourceHandle : public RDGNodeHandle {};
struct RDGBufferHandle : public RDGResourceHandle {};
struct RDGTextureHandle : public RDGResourceHandle {};
struct RDGPassHandle : public RDGNodeHandle {};

template <typename T>
requires std::is_base_of_v<radray::RDGNodeHandle, T>
struct RDGHandleHash {
    inline std::size_t operator()(const T& h) const noexcept {
        return std::hash<uint64_t>{}(h.Id);
    }
};

}  // namespace radray

template <typename T>
requires std::is_base_of_v<radray::RDGNodeHandle, T>
struct std::hash<T> : public radray::RDGHandleHash<T> {};

namespace radray {

class RenderGraph;
class RDGNode;
class RDGEdge;
class IRDGRasterPass;
class IRDGComputePass;

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

enum class RDGEdgeTag : uint32_t {
    UNKNOWN = 0x0,
    ResourceDependency = 0x1,
    PassDependency = ResourceDependency << 1,
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
struct is_compound_enum_flags<RDGNodeTag> : std::true_type {};

template <>
struct is_flags<RDGEdgeTag> : public std::true_type {};
template <>
struct is_compound_enum_flags<RDGEdgeTag> : std::true_type {};

template <>
struct is_flags<RDGExecutionStage> : public std::true_type {};
template <>
struct is_flags<RDGMemoryAccess> : public std::true_type {};

using RDGNodeTags = EnumFlags<RDGNodeTag>;
using RDGEdgeTags = EnumFlags<RDGEdgeTag>;
using RDGExecutionStages = EnumFlags<RDGExecutionStage>;
using RDGMemoryAccesses = EnumFlags<RDGMemoryAccess>;

bool IsReadAccessFlag(RDGMemoryAccess access) noexcept;
bool IsWriteAccessFlag(RDGMemoryAccess access) noexcept;
bool HasReadAccess(RDGMemoryAccesses access) noexcept;
bool HasWriteAccess(RDGMemoryAccesses access) noexcept;
bool IsReadOnlyAccess(RDGMemoryAccesses access) noexcept;
RDGMemoryAccesses AllowedAccessesForStage(RDGExecutionStage stage) noexcept;
RDGMemoryAccesses AllowedAccessesForStages(RDGExecutionStages stages) noexcept;
bool AreLayoutsCompatible(RDGTextureLayout lhs, RDGTextureLayout rhs) noexcept;
bool IsTextureLayoutCompatibleWithAccess(RDGTextureLayout layout, RDGMemoryAccesses access) noexcept;

struct RDGBufferState {
    RDGExecutionStages Stage{RDGExecutionStage::NONE};
    RDGMemoryAccesses Access{RDGMemoryAccess::NONE};
    render::BufferRange Range{};

    bool HasWrite() const noexcept;
};

struct RDGTextureState {
    RDGExecutionStages Stage{RDGExecutionStage::NONE};
    RDGMemoryAccesses Access{RDGMemoryAccess::NONE};
    RDGTextureLayout Layout{RDGTextureLayout::UNKNOWN};
    render::SubresourceRange Range{};

    bool HasWrite() const noexcept;
};

struct RDGColorAttachmentRecord {
    RDGTextureHandle Texture{};
    uint32_t Slot{0};
    render::SubresourceRange Range{};
    render::LoadAction Load{render::LoadAction::DontCare};
    render::StoreAction Store{render::StoreAction::Store};
    std::optional<render::ColorClearValue> ClearValue{};

    bool HasWriteAccess() const noexcept;
};

struct RDGDepthStencilAttachmentRecord {
    RDGTextureHandle Texture{};
    render::SubresourceRange Range{};
    render::LoadAction DepthLoad{render::LoadAction::DontCare};
    render::StoreAction DepthStore{render::StoreAction::Store};
    render::LoadAction StencilLoad{render::LoadAction::DontCare};
    render::StoreAction StencilStore{render::StoreAction::Store};
    std::optional<render::DepthStencilClearValue> ClearValue{};

    bool HasWriteAccess() const noexcept;
};

struct RDGBufferRecord {
    RDGBufferHandle Buffer{};
    RDGBufferState State{};
};

struct RDGTextureRecord {
    RDGTextureHandle Texture{};
    RDGTextureState State{};
};

struct RDGCopyBufferToBufferRecord {
    RDGBufferHandle Dst{};
    uint64_t DstOffset{0};
    RDGBufferHandle Src{};
    uint64_t SrcOffset{0};
    uint64_t Size{0};
};

struct RDGCopyBufferToTextureRecord {
    RDGTextureHandle Dst{};
    render::SubresourceRange DstRange{};
    RDGBufferHandle Src{};
    uint64_t SrcOffset{0};
};

struct RDGCopyTextureToBufferRecord {
    RDGBufferHandle Dst{};
    uint64_t DstOffset{0};
    RDGTextureHandle Src{};
    render::SubresourceRange SrcRange{};
};

using RDGCopyRecord = std::variant<RDGCopyBufferToBufferRecord, RDGCopyBufferToTextureRecord, RDGCopyTextureToBufferRecord>;

class RDGNode {
public:
    RDGNode(uint64_t id, std::string_view name) noexcept
        : _name(name),
          _id(id) {}
    virtual ~RDGNode() noexcept = default;

    virtual RDGNodeTags GetTag() const noexcept = 0;

    RDGNodeHandle GetHandle() const noexcept { return RDGNodeHandle{_id}; }

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
};

class RDGBufferNode final : public RDGResourceNode {
public:
    using RDGResourceNode::RDGResourceNode;
    virtual ~RDGBufferNode() noexcept = default;

    RDGNodeTags GetTag() const noexcept override { return RDGNodeTag::Buffer; }

public:
    uint64_t _size{0};
    render::MemoryType _memory{render::MemoryType::Device};
    render::BufferUses _usage{render::BufferUse::UNKNOWN};
    GpuBufferHandle _importBuffer{GpuBufferHandle::Invalid()};
    std::optional<RDGBufferState> _importState;
    std::optional<RDGBufferState> _exportState;
};

class RDGTextureNode final : public RDGResourceNode {
public:
    using RDGResourceNode::RDGResourceNode;
    virtual ~RDGTextureNode() noexcept = default;

    RDGNodeTags GetTag() const noexcept override { return RDGNodeTag::Texture; }

public:
    render::TextureDimension _dim{render::TextureDimension::UNKNOWN};
    uint32_t _width{0};
    uint32_t _height{0};
    uint32_t _depthOrArraySize{0};
    uint32_t _mipLevels{0};
    uint32_t _sampleCount{0};
    render::TextureFormat _format{render::TextureFormat::UNKNOWN};
    render::MemoryType _memory{render::MemoryType::Device};
    render::TextureUses _usage{render::TextureUse::UNKNOWN};
    GpuTextureHandle _importTexture{GpuTextureHandle::Invalid()};
    std::optional<RDGTextureState> _importState;
    std::optional<RDGTextureState> _exportState;
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
    virtual ~RDGGraphicsPassNode() noexcept;

    RDGNodeTags GetTag() const noexcept override { return RDGNodeTag::GraphicsPass; }

public:
    unique_ptr<IRDGRasterPass> _impl{};
    vector<RDGColorAttachmentRecord> _colorAttachments;
    std::optional<RDGDepthStencilAttachmentRecord> _depthStencilAttachment;
};

class RDGComputePassNode final : public RDGPassNode {
public:
    using RDGPassNode::RDGPassNode;
    virtual ~RDGComputePassNode() noexcept;

    RDGNodeTags GetTag() const noexcept override { return RDGNodeTag::ComputePass; }

public:
    unique_ptr<IRDGComputePass> _impl{};
};

class RDGCopyPassNode final : public RDGPassNode {
public:
    using RDGPassNode::RDGPassNode;
    virtual ~RDGCopyPassNode() noexcept = default;

    RDGNodeTags GetTag() const noexcept override { return RDGNodeTag::CopyPass; }

public:
    vector<RDGCopyRecord> _copys;
};

class RDGEdge {
public:
    RDGEdge(
        RDGNode* from,
        RDGNode* to) noexcept
        : _from(from),
          _to(to) {}

    virtual ~RDGEdge() noexcept = default;

    virtual RDGEdgeTags GetTag() const noexcept = 0;

public:
    RDGNode* _from{nullptr};
    RDGNode* _to{nullptr};
};

class RDGResourceDependencyEdge final : public RDGEdge {
public:
    RDGResourceDependencyEdge(
        RDGNode* from,
        RDGNode* to,
        RDGExecutionStages stage,
        RDGMemoryAccesses access) noexcept
        : RDGEdge(from, to),
          _stage(stage),
          _access(access) {}

    RDGEdgeTags GetTag() const noexcept override { return RDGEdgeTag::ResourceDependency; }

public:
    RDGExecutionStages _stage{RDGExecutionStage::NONE};
    RDGMemoryAccesses _access{RDGMemoryAccess::NONE};
    render::BufferRange _bufferRange;
    RDGTextureLayout _textureLayout{RDGTextureLayout::UNKNOWN};
    render::SubresourceRange _textureRange;
};

class RDGPassDependencyEdge final : public RDGEdge {
public:
    RDGPassDependencyEdge(
        RDGNode* from,
        RDGNode* to) noexcept
        : RDGEdge(from, to) {}

    RDGEdgeTags GetTag() const noexcept override { return RDGEdgeTag::PassDependency; }
};

class IRDGRasterPass {
public:
    class Builder {
    public:
        void Build(RenderGraph* graph, RDGGraphicsPassNode* node);

        Builder& UseColorAttachment(
            uint32_t slot,
            RDGTextureHandle texture,
            render::SubresourceRange range,
            render::LoadAction load,
            render::StoreAction store,
            std::optional<render::ColorClearValue> clearValue);
        Builder& UseDepthStencilAttachment(
            RDGTextureHandle texture,
            render::SubresourceRange range,
            render::LoadAction depthLoad, render::StoreAction depthStore,
            render::LoadAction stencilLoad, render::StoreAction stencilStore,
            std::optional<render::DepthStencilClearValue> clearValue);

        Builder& UseVertexBuffer(RDGBufferHandle buffer, render::BufferRange range);
        Builder& UseIndexBuffer(RDGBufferHandle buffer, render::BufferRange range);
        Builder& UseIndirectBuffer(RDGBufferHandle buffer, render::BufferRange range);
        Builder& UseCBuffer(RDGBufferHandle buffer, render::ShaderStages stages, render::BufferRange range);
        Builder& UseBuffer(RDGBufferHandle buffer, render::ShaderStages stages, render::BufferRange range);
        Builder& UseRWBuffer(RDGBufferHandle buffer, render::ShaderStages stages, render::BufferRange range);

        Builder& UseTexture(RDGTextureHandle texture, render::ShaderStages stages, render::SubresourceRange range);
        Builder& UseRWTexture(RDGTextureHandle texture, render::ShaderStages stages, render::SubresourceRange range);

    public:
        vector<RDGBufferRecord> _buffers{};
        vector<RDGTextureRecord> _textures{};
        vector<RDGColorAttachmentRecord> _colorAttachments{};
        std::optional<RDGDepthStencilAttachmentRecord> _depthStencilAttachment{};
    };

    virtual ~IRDGRasterPass() noexcept = default;
    virtual void Setup(Builder& builder) = 0;
    virtual void Execute(render::GraphicsCommandEncoder* encoder, GpuAsyncContext* context) = 0;
};

class IRDGComputePass {
public:
    class Builder {
    public:
        void Build(RenderGraph* graph, RDGComputePassNode* node);

        Builder& UseCBuffer(RDGBufferHandle buffer, render::BufferRange range);
        Builder& UseBuffer(RDGBufferHandle buffer, render::BufferRange range);
        Builder& UseRWBuffer(RDGBufferHandle buffer, render::BufferRange range);

        Builder& UseTexture(RDGTextureHandle texture, render::SubresourceRange range);
        Builder& UseRWTexture(RDGTextureHandle texture, render::SubresourceRange range);

    public:
        vector<RDGBufferRecord> _buffers{};
        vector<RDGTextureRecord> _textures{};
    };

    virtual ~IRDGComputePass() noexcept = default;
    virtual void Setup(Builder& builder) = 0;
    virtual void Execute(render::ComputeCommandEncoder* encoder, GpuAsyncContext* context) = 0;
};

class RDGCopyPassBuilder {
public:
    RDGPassHandle Build(RenderGraph* graph);

    RDGCopyPassBuilder& SetName(std::string_view name);
    RDGCopyPassBuilder& CopyBufferToBuffer(RDGBufferHandle dst, uint64_t dstOffset, RDGBufferHandle src, uint64_t srcOffset, uint64_t size);
    RDGCopyPassBuilder& CopyBufferToTexture(RDGTextureHandle dst, render::SubresourceRange dstRange, RDGBufferHandle src, uint64_t srcOffset);
    RDGCopyPassBuilder& CopyTextureToBuffer(RDGBufferHandle dst, uint64_t dstOffset, RDGTextureHandle src, render::SubresourceRange srcRange);

public:
    string _name;
    vector<RDGCopyRecord> _copys;
    vector<RDGBufferRecord> _buffers{};
    vector<RDGTextureRecord> _textures{};
};

class RenderGraph {
public:
    struct ValidateResult {
        bool IsValid;
        string Message;
    };

    class CompileResult {
    public:
        struct BufferBarrier {
            RDGBufferHandle Buffer{};
            RDGExecutionStages SrcStage{RDGExecutionStage::NONE};
            RDGMemoryAccesses SrcAccess{RDGMemoryAccess::NONE};
            RDGExecutionStages DstStage{RDGExecutionStage::NONE};
            RDGMemoryAccesses DstAccess{RDGMemoryAccess::NONE};
            render::BufferRange Range{};
        };

        struct TextureBarrier {
            RDGTextureHandle Texture{};
            RDGExecutionStages SrcStage{RDGExecutionStage::NONE};
            RDGMemoryAccesses SrcAccess{RDGMemoryAccess::NONE};
            RDGTextureLayout SrcLayout{RDGTextureLayout::Undefined};
            RDGExecutionStages DstStage{RDGExecutionStage::NONE};
            RDGMemoryAccesses DstAccess{RDGMemoryAccess::NONE};
            RDGTextureLayout DstLayout{RDGTextureLayout::Undefined};
            render::SubresourceRange Range{};
        };

        struct BarrierBatch {
            vector<BufferBarrier> BufferBarriers{};
            vector<TextureBarrier> TextureBarriers{};
        };

        string ExportCompiledGraphviz() const;
        string ExportExecutionGraphviz() const;
    };

    // RDG 创建的资源
    RDGBufferHandle AddBuffer(uint64_t size, render::MemoryType memory, render::BufferUses usage, std::string_view name);
    RDGTextureHandle AddTexture(
        render::TextureDimension dim,
        uint32_t width, uint32_t height,
        uint32_t depthOrArraySize, uint32_t mipLevels,
        uint32_t sampleCount,
        render::TextureFormat format,
        render::MemoryType memory,
        render::TextureUses usage,
        std::string_view name);
    // 导入外部资源
    RDGBufferHandle ImportBuffer(GpuBufferHandle buffer, RDGExecutionStages stage, RDGMemoryAccesses access, render::BufferRange bufferRange, std::string_view name);
    RDGTextureHandle ImportTexture(GpuTextureHandle texture, RDGExecutionStages stage, RDGMemoryAccesses access, RDGTextureLayout layout, render::SubresourceRange textureRange, std::string_view name);
    // 最终要导出的资源
    void ExportBuffer(RDGBufferHandle node, RDGExecutionStages stage, RDGMemoryAccesses access, render::BufferRange bufferRange);
    void ExportTexture(RDGTextureHandle node, RDGExecutionStages stage, RDGMemoryAccesses access, RDGTextureLayout layout, render::SubresourceRange textureRange);
    // Pass
    RDGPassHandle AddRasterPass(std::string_view name, unique_ptr<IRDGRasterPass> pass);
    RDGPassHandle AddComputePass(std::string_view name, unique_ptr<IRDGComputePass> pass);
    RDGPassHandle AddCopyPass(std::string_view name);
    // 图连接
    RDGEdge* Link(RDGNodeHandle from, RDGNodeHandle to, RDGExecutionStages stage, RDGMemoryAccesses access, render::BufferRange bufferRange);
    RDGEdge* Link(RDGNodeHandle from, RDGNodeHandle to, RDGExecutionStages stage, RDGMemoryAccesses access, RDGTextureLayout layout, render::SubresourceRange textureRange);
    void AddPassDependency(RDGPassHandle before, RDGPassHandle after);

    ValidateResult Validate() const;
    CompileResult Compile() const;
    string ExportGraphviz() const;

    RDGNode* Resolve(RDGNodeHandle handle) const;

    static RDGExecutionStages ShaderStagesToExecStages(render::ShaderStages stages) noexcept;

public:
    vector<unique_ptr<RDGNode>> _nodes;
    vector<unique_ptr<RDGEdge>> _edges;
};

std::string_view format_as(RDGNodeTag v) noexcept;
std::string_view format_as(RDGEdgeTag v) noexcept;
std::string_view format_as(RDGExecutionStage v) noexcept;
std::string_view format_as(RDGMemoryAccess v) noexcept;
std::string_view format_as(RDGTextureLayout v) noexcept;
std::string_view format_as(RDGResourceOwnership v) noexcept;

}  // namespace radray

/*
# RenderGraph 校验规则

## 1. 节点基础校验

| # | 检查项 | 说明 |
|---|---|---|
| 1.1 | **Handle 有效** | 每个节点 `_id` 在 `[0, _nodes.size())` 范围内 |
| 1.2 | **Id 与索引一致** | `_nodes[i]->_id == i` |
| 1.3 | **Tag 具体化** | Resource 节点的 Tag 必须是 `Buffer` 或 `Texture`（不能是纯 `Resource`）；Pass 节点的 Tag 必须是 `GraphicsPass`/`ComputePass`/`CopyPass`（不能是纯 `Pass`） |

## 2. 边基础校验

| # | 检查项 | 说明 |
|---|---|---|
| 2.1 | **两端非空** | 每条 `RDGEdge` 的 `_from` 和 `_to` 不为 `nullptr` |
| 2.2 | **两端存在** | `_from` 和 `_to` 指向的节点在 `_nodes` 中 |
| 2.3 | **Tag 具体化** | `GetTag()` 必须是 `ResourceDependency` 或 `PassDependency`，不能是 `UNKNOWN` |
| 2.4 | **双向登记一致** | 每条边同时出现在 `_from->_outEdges` 和 `_to->_inEdges` 中；反之亦然 |
| 2.5 | **不自环** | `_from != _to` |

## 3. ResourceDependencyEdge 连接规则

| # | 检查项 | 说明 |
|---|---|---|
| 3.1 | **二分图约束** | `ResourceDependencyEdge` 两端必须是「一个 Resource 节点 + 一个 Pass 节点」，不允许 Pass↔Pass 或 Resource↔Resource |
| 3.2 | **读方向：Resource→Pass** | 若 Access 仅含读标志（`VertexRead`/`IndexRead`/`ConstantRead`/`ShaderRead`/`ColorAttachmentRead`/`DepthStencilRead`/`TransferRead`/`HostRead`/`IndirectRead`），方向必须是 Resource→Pass |
| 3.3 | **写方向：Pass→Resource** | 若 Access 含写标志（`ShaderWrite`/`ColorAttachmentWrite`/`DepthStencilWrite`/`TransferWrite`/`HostWrite`），方向必须是 Pass→Resource |
| 3.4 | **Buffer 边匹配 Buffer 节点** | 若边有 `_bufferRange`（非全默认值）且 `_textureLayout == UNKNOWN`，关联的 Resource 端必须是 `RDGBufferNode` |
| 3.5 | **Texture 边匹配 Texture 节点** | 若边有 `_textureLayout != UNKNOWN` 或 `_textureRange` 非默认值，关联的 Resource 端必须是 `RDGTextureNode` |
| 3.6 | **Stage 非 NONE** | `_stage` 不能是 `RDGExecutionStage::NONE` |
| 3.7 | **Access 非 NONE** | `_access` 不能是 `RDGMemoryAccess::NONE` |
| 3.8 | **无重复边** | 同一对 `(from, to)` 节点之间不应有 stage + access + range 完全相同的多条 `ResourceDependencyEdge` |

## 4. PassDependencyEdge 连接规则

| # | 检查项 | 说明 |
|---|---|---|
| 4.1 | **两端都是 Pass** | `PassDependencyEdge` 的 `_from` 和 `_to` 必须都是 `RDGPassNode` 的子类（Tag 含 `Pass` 标志） |
| 4.2 | **不自依赖** | `_from` 和 `_to` 不能是同一个 Pass |
| 4.3 | **不重复** | 同一对 `(from, to)` Pass 之间最多一条 `PassDependencyEdge` |

## 5. 全局图结构校验

| # | 检查项 | 说明 |
|---|---|---|
| 5.1 | **无环（DAG）** | 整图（包含两种边）不能存在有向环。拓扑排序检测 |
| 5.2 | **Pass 可达性** | 从 Import 资源（或无入边的资源）出发，图中所有 Pass 都应可达 |
| 5.3 | **无孤立 Pass** | 每个 Pass 至少有一条 `ResourceDependencyEdge`（入或出）；纯 `PassDependencyEdge` 连接但没有任何资源交互的 Pass 无法执行有意义的工作 |
| 5.4 | **无孤立 Resource** | 每个 Resource 至少有一条 `ResourceDependencyEdge`（被至少一个 Pass 引用） |

## 6. Resource 节点校验

### 6.1 通用
| # | 检查项 | 说明 |
|---|---|---|
| 6.1.1 | **Ownership 合法** | 不能是 `UNKNOWN` |
| 6.1.2 | **External 需 Import** | `ownership == External` 时，必须有 importState |
| 6.1.3 | **Internal 无 Import** | `ownership == Internal` 时，不应有 importBuffer/importTexture 和 importState |

### 6.2 Buffer 节点
| # | 检查项 | 说明 |
|---|---|---|
| 6.2.1 | **Size > 0** | Internal Buffer 的 `_size` 必须 > 0 |
| 6.2.2 | **Usage 非 UNKNOWN** | `_usage` 不能是 `BufferUse::UNKNOWN` |
| 6.2.3 | **Import Handle 有效** | External Buffer 的 `_importBuffer` 必须有效 |
| 6.2.4 | **BufferRange 不越界** | 所有引用此 Buffer 的 `ResourceDependencyEdge` 上的 `_bufferRange`，`Offset + Size` 不超过 `_size`（Size 为 `All()` 除外） |

### 6.3 Texture 节点
| # | 检查项 | 说明 |
|---|---|---|
| 6.3.1 | **维度合法** | `_dim != UNKNOWN` |
| 6.3.2 | **尺寸有效** | `_width > 0 && _height > 0 && _depthOrArraySize > 0 && _mipLevels > 0 && _sampleCount > 0` |
| 6.3.3 | **格式合法** | `_format != TextureFormat::UNKNOWN` |
| 6.3.4 | **Usage 非 UNKNOWN** | `_usage != TextureUse::UNKNOWN` |
| 6.3.5 | **Import Handle 有效** | External Texture 的 `_importTexture` 必须有效 |
| 6.3.6 | **SubresourceRange 不越界** | 所有引用此 Texture 的边上 `BaseArrayLayer + ArrayLayerCount <= _depthOrArraySize`，`BaseMipLevel + MipLevelCount <= _mipLevels`（`All` 除外） |

## 7. Pass 节点校验

### 7.1 通用
| # | 检查项 | 说明 |
|---|---|---|
| 7.1.1 | **Pass 至少有一个写输出** | 每个 Pass 至少有一条出边（`ResourceDependencyEdge` 方向 Pass→Resource，含写 Access），否则 Pass 无副作用 |

### 7.2 GraphicsPass
| # | 检查项 | 说明 |
|---|---|---|
| 7.2.1 | **`_impl` 非空** | `_impl != nullptr` |
| 7.2.2 | **Color Attachment Slot 唯一** | `_colorAttachments` 中各 `Slot` 不重复 |
| 7.2.3 | **Color Attachment Slot 连续** | Slot 值从 0 开始连续无空洞 |
| 7.2.4 | **Color Attachment Handle 有效** | 每个 `Texture` Handle 有效且对应节点是 `RDGTextureNode` |
| 7.2.5 | **Color Attachment 非深度格式** | 引用的 Texture 格式不能是深度/模板格式 |
| 7.2.6 | **DepthStencil 是深度格式** | 若有 `_depthStencilAttachment`，Texture 格式必须是深度/模板格式 |
| 7.2.7 | **Color 与 DepthStencil 不冲突** | 同一 Texture 不能同时出现在 Color Attachment 和 DepthStencil Attachment 中 |
| 7.2.8 | **Clear 需 ClearValue** | `Load == Clear` 时 `ClearValue` 必须有值 |
| 7.2.9 | **边 Stage 范围** | 此 Pass 的 `ResourceDependencyEdge` 的 Stage 只含 `VertexInput`/`VertexShader`/`PixelShader`/`DepthStencil`/`ColorOutput`/`Indirect`，不含 `ComputeShader`/`Copy` |

### 7.3 ComputePass
| # | 检查项 | 说明 |
|---|---|---|
| 7.3.1 | **`_impl` 非空** | `_impl != nullptr` |
| 7.3.2 | **边 Stage = ComputeShader** | 此 Pass 的所有 `ResourceDependencyEdge` 的 Stage 只能是 `ComputeShader` |
| 7.3.3 | **Access 无图形管线参数** | 不应出现 `VertexRead`/`IndexRead`/`ColorAttachmentRead`/`ColorAttachmentWrite`/`DepthStencilRead`/`DepthStencilWrite`/`IndirectRead` |

### 7.4 CopyPass
| # | 检查项 | 说明 |
|---|---|---|
| 7.4.1 | **`_copys` 非空** | 至少一条 copy 记录 |
| 7.4.2 | **边 Stage = Copy** | 此 Pass 的所有 `ResourceDependencyEdge` 的 Stage 只能是 `Copy` |
| 7.4.3 | **Access 限 Transfer** | 只能是 `TransferRead` 或 `TransferWrite` |
| 7.4.4 | **Copy Record Handle 有效** | 每条 `RDGCopyRecord` 中的 Src/Dst Handle 有效且节点类型匹配 |
| 7.4.5 | **Copy Src ≠ Dst** | Buffer→Buffer 时 Src 和 Dst 不能是同一节点（或范围无重叠） |

## 8. Import / Export 校验

| # | 检查项 | 说明 |
|---|---|---|
| 8.1 | **Import Stage/Access 非 NONE** | Import 的 `Stage` 和 `Access` 至少指定一种 |
| 8.2 | **Import Texture Layout 非 UNKNOWN** | Import Texture 的 Layout 不能是 `UNKNOWN` |
| 8.3 | **Export Stage/Access 非 NONE** | Export 的 `Stage` 和 `Access` 至少指定一种 |
| 8.4 | **Export Texture Layout 非 UNKNOWN** | Export Texture 的 Layout 不能是 `UNKNOWN` |
| 8.5 | **Export 必须有写入源** | 被 Export 的 Resource（Internal）必须至少有一条 Pass→Resource 写入边，否则导出的内容未定义 |
| 8.6 | **Export Handle 存在** | Export 的目标 Handle 必须在图中存在且类型正确 |

## 9. Stage × Access 兼容性

| Stage | 允许的 Access |
|---|---|
| `VertexInput` | `VertexRead`, `IndexRead` |
| `VertexShader` | `ConstantRead`, `ShaderRead`, `ShaderWrite` |
| `PixelShader` | `ConstantRead`, `ShaderRead`, `ShaderWrite` |
| `DepthStencil` | `DepthStencilRead`, `DepthStencilWrite` |
| `ColorOutput` | `ColorAttachmentRead`, `ColorAttachmentWrite` |
| `Indirect` | `IndirectRead` |
| `ComputeShader` | `ConstantRead`, `ShaderRead`, `ShaderWrite` |
| `Copy` | `TransferRead`, `TransferWrite` |
| `Host` | `HostRead`, `HostWrite` |
| `Present` | 无 Access（或仅只读语义） |

当 Stage 是组合标志（如 `VertexShader | PixelShader`）时，Access 必须是对应 Stage 允许集合的**并集**子集。

## 10. Layout 一致性校验（Texture 特有）

| # | 检查项 | 说明 |
|---|---|---|
| 10.1 | **同 Pass 内同 Subresource 不能有不兼容 Layout** | 同一个 Pass 引用同一 Texture 的重叠 SubresourceRange 时，所有 `ResourceDependencyEdge` 上的 `_textureLayout` 必须相同（或其中一个是 `General`） |
| 10.2 | **Layout 与 Access 匹配** | `ColorAttachment` ↔ `ColorAttachmentRead/Write`；`DepthStencilAttachment` ↔ `DepthStencilRead/Write`；`DepthStencilReadOnly` ↔ `DepthStencilRead`；`ShaderReadOnly` ↔ `ShaderRead`/`ConstantRead`；`General` ↔ 任意读写；`TransferSource` ↔ `TransferRead`；`TransferDestination` ↔ `TransferWrite`；`Present` ↔ 无写入或只读 |
| 10.3 | **Import→Export Layout 连贯** | 如果 Texture 同时有 Import 和 Export，且中间没有 Pass 使用它，Import Layout 应与 Export Layout 一致（否则缺少转换） |

## 11. 数据冒险检测（Hazard）

| # | 检查项 | 说明 |
|---|---|---|
| 11.1 | **Write-After-Write** | 同一 Resource 的重叠范围（Buffer: BufferRange 重叠；Texture: SubresourceRange 重叠）不能被**多个 Pass 写入**，除非这些 Pass 之间有拓扑序保证（通过 ResourceDependencyEdge 中转或 PassDependencyEdge 直连） |
| 11.2 | **Read-After-Write** | 读取 Pass 必须在写入 Pass 之后（拓扑序）。Internal Resource 被读但无任何写入来源→报错（数据未初始化） |
| 11.3 | **Write-After-Read** | 写入 Pass 必须在读取 Pass 之后（拓扑序），否则会覆盖未消费的数据 |

检测方法：对每个 Resource 收集所有关联的 `ResourceDependencyEdge`，按边方向分为读集合（Resource→Pass）和写集合（Pass→Resource），然后：
- 写集合内的每对 Pass 之间需有拓扑序关系（通过 `PassDependencyEdge` 或资源中转可达）
- 写集合与读集合之间的每对 Pass 之间需有拓扑序关系
- 范围不重叠的访问可以豁免

## 12. PassDependencyEdge 特定校验

| # | 检查项 | 说明 |
|---|---|---|
| 12.1 | **不冗余** | 如果两个 Pass 已经通过 ResourceDependencyEdge（经由某个 Resource 中转）有明确的先后关系，额外的 PassDependencyEdge 虽不违规，但可以**警告**冗余 |
| 12.2 | **不矛盾** | PassDependencyEdge 指定的顺序不应与 ResourceDependencyEdge 推导出的拓扑序相反（若 A 通过资源依赖必须在 B 之后，则不能有 `A→B` 的 PassDependencyEdge） |
| 12.3 | **环检测含 PassDependency** | DAG 检测必须将 PassDependencyEdge 纳入。Resource 中转边 `Pass→Resource→Pass` 等价于 Pass 间一条间接边，与 PassDependencyEdge 合并后做环检测 |
*/
