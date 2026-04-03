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
struct is_flags<RDGExecutionStage> : public std::true_type {};
template <>
struct is_flags<RDGMemoryAccess> : public std::true_type {};

using RDGNodeTags = EnumFlags<RDGNodeTag>;
using RDGExecutionStages = EnumFlags<RDGExecutionStage>;
using RDGMemoryAccesses = EnumFlags<RDGMemoryAccess>;

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
        RDGNode* to,
        RDGExecutionStages stage,
        RDGMemoryAccesses access) noexcept
        : _from(from),
          _to(to),
          _stage(stage),
          _access(access) {}

public:
    RDGNode* _from{nullptr};
    RDGNode* _to{nullptr};
    RDGExecutionStages _stage{RDGExecutionStage::NONE};
    RDGMemoryAccesses _access{RDGMemoryAccess::NONE};
    render::BufferRange _bufferRange;
    RDGTextureLayout _textureLayout{RDGTextureLayout::UNKNOWN};
    render::SubresourceRange _textureRange;
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
        string ExportCompiledGraphviz() const;
        string ExportExecutionGraphviz() const;
    };

    // ---------------- Core ----------------
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
    RDGEdge* CreateEdge(RDGNode* from, RDGNode* to, RDGExecutionStages stage, RDGMemoryAccesses access);

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
std::string_view format_as(RDGExecutionStage v) noexcept;
std::string_view format_as(RDGMemoryAccess v) noexcept;
std::string_view format_as(RDGTextureLayout v) noexcept;
std::string_view format_as(RDGResourceOwnership v) noexcept;

}  // namespace radray
