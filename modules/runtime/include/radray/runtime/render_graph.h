#pragma once

#include <numeric>

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
};

struct RDGResourceHandle : public RDGNodeHandle {};
struct RDGBufferHandle : public RDGResourceHandle {};
struct RDGTextureHandle : public RDGResourceHandle {};
struct RDGPassHandle : public RDGNodeHandle {};

class RDGNode {
public:
    virtual ~RDGNode() noexcept = default;

    virtual RDGNodeTags GetTag() const noexcept = 0;

public:
    string _name;
    uint64_t _id;
    vector<RDGEdge*> _inEdges;
    vector<RDGEdge*> _outEdges;
};

class RDGResourceNode : public RDGNode {
public:
    virtual ~RDGResourceNode() noexcept = default;

    RDGNodeTags GetTag() const noexcept override { return RDGNodeTag::Resource; }

public:
    RDGResourceOwnership _ownership;
};

class RDGBufferNode final : public RDGResourceNode {
public:
    virtual ~RDGBufferNode() noexcept = default;

    RDGNodeTags GetTag() const noexcept override { return RDGNodeTag::Buffer; }

public:
    uint64_t _size{0};
};

class RDGTextureNode final : public RDGResourceNode {
public:
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
};

class RDGPassNode : public RDGNode {
public:
    virtual ~RDGPassNode() noexcept = default;

    RDGNodeTags GetTag() const noexcept override { return RDGNodeTag::Pass; }

public:
    render::QueueType _type;
};

class RDGEdge {
public:
    RDGEdge() noexcept = default;

public:
    RDGNode* _from;
    RDGNode* _to;
    RDGExecutionStage _stage;
    RDGMemoryAccess _access;
    render::BufferRange _bufferRange;
    RDGTextureLayout _textureLayout;
    render::SubresourceRange _textureRange;
};

class RDGRasterPassBuilder {
public:
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
    RenderGraph* _graph{nullptr};
};

class RDGComputePassBuilder {
public:
    RDGPassHandle Build();

    RDGComputePassBuilder& UseCBuffer(RDGBufferHandle buffer, render::BufferRange range);
    RDGComputePassBuilder& UseBuffer(RDGBufferHandle buffer, render::BufferRange range);
    RDGComputePassBuilder& UseRWBuffer(RDGBufferHandle buffer, render::BufferRange range);

    RDGComputePassBuilder& UseTexture(RDGTextureHandle texture, render::SubresourceRange range);
    RDGComputePassBuilder& UseRWTexture(RDGTextureHandle texture, render::SubresourceRange range);

public:
    RenderGraph* _graph{nullptr};
};

class RDGCopyPassBuilder {
public:
    RDGPassHandle Build();

    RDGCopyPassBuilder& CopyBufferToBuffer(RDGBufferHandle dst, uint64_t dstOffset, RDGBufferHandle src, uint64_t srcOffset, uint64_t size);
    RDGCopyPassBuilder& CopyBufferToTexture(RDGTextureHandle dst, render::SubresourceRange dstRange, RDGBufferHandle src, uint64_t srcOffset);
    RDGCopyPassBuilder& CopyTextureToBuffer(RDGBufferHandle dst, uint64_t dstOffset, RDGTextureHandle src, render::SubresourceRange srcRange);

public:
    RenderGraph* _graph{nullptr};
};

class RenderGraph {
public:
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
    RDGPassHandle AddPass(std::string_view name);
    // 图连接
    void Link(RDGNodeHandle from, RDGNodeHandle to, RDGExecutionStage stage, RDGMemoryAccess access, render::BufferRange bufferRange);
    void Link(RDGNodeHandle from, RDGNodeHandle to, RDGExecutionStage stage, RDGMemoryAccess access, RDGTextureLayout layout, render::SubresourceRange textureRange);

public:
    GpuRuntime* _gpu{nullptr};
    vector<unique_ptr<RDGNode>> _nodes;
    vector<unique_ptr<RDGEdge>> _edges;
};

}  // namespace radray
