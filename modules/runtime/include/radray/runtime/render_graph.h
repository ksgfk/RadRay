#pragma once

#include <numeric>

#include <radray/types.h>
#include <radray/enum_flags.h>
#include <radray/render/common.h>
#include <radray/runtime/gpu_system.h>

namespace radray {

enum class RDGNodeTag {
    UNKNOWN = 0x0,
    Resource = 0x1,
    Buffer = Resource | (Resource << 1),
    Texture = Resource | (Resource << 2),
    Pass = Resource << 3,
};

enum class RDGExecutionStage {
    None,
    // TODO:
};

enum class RDGMemoryAccess {
    UNKNOWN,
    // TODO:
};

enum class RDGTextureLayout {
    UNKNOWN,
    // TODO:
};

enum class RDGResourceOwnership {
    UNKNOWN,
    External,
    Internal,
    Transient
};

template <>
struct is_flags<RDGNodeTag> : public std::true_type {};

using RDGNodeTags = EnumFlags<RDGNodeTag>;

struct RDGNodeHandle {
    uint64_t Id{std::numeric_limits<uint64_t>::max()};
};

struct RDGResourceHandle : public RDGNodeHandle {};
struct RDGBufferHandle : public RDGResourceHandle {};
struct RDGTextureHandle : public RDGResourceHandle {};
struct RDGPassHandle : public RDGNodeHandle {};

class RDGEdge;

class RDGNode {
public:
    virtual ~RDGNode() noexcept = default;

    virtual RDGNodeTags GetTag() const noexcept = 0;

public:
    string _name;
    uint64_t _id;
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
};

class RDGEdge {
public:
    RDGNode* _from;
    RDGNode* _to;
    RDGExecutionStage _stage;
    RDGMemoryAccess _access;
    render::BufferRange _bufferRange;
    RDGTextureLayout _textureLayout;
    render::SubresourceRange _textureRange;
};

class RenderGraph {
public:
    RDGBufferHandle AddBuffer(uint64_t size, std::string_view name);
    RDGTextureHandle AddTexture(
        render::TextureDimension dim,
        uint32_t width, uint32_t height,
        uint32_t depthOrArraySize, uint32_t mipLevels,
        uint32_t sampleCount,
        render::TextureFormat format,
        std::string_view name);

    RDGBufferHandle ImportBuffer(GpuBufferHandle buffer, std::string_view name);
    RDGTextureHandle ImportTexture(GpuTextureHandle texture, std::string_view name);

    void ExportBuffer(RDGBufferHandle node, RDGExecutionStage stage, RDGMemoryAccess access, render::BufferRange bufferRange);
    void ExportTexture(RDGTextureHandle node, RDGExecutionStage stage, RDGMemoryAccess access, RDGTextureLayout layout, render::SubresourceRange textureRange);

    RDGPassHandle AddPass(std::string_view name);

    void Link(RDGNodeHandle from, RDGNodeHandle to, RDGExecutionStage stage, RDGMemoryAccess access, render::BufferRange bufferRange);
    void Link(RDGNodeHandle from, RDGNodeHandle to, RDGExecutionStage stage, RDGMemoryAccess access, RDGTextureLayout layout, render::SubresourceRange textureRange);

    GpuRuntime* _gpu{nullptr};
    vector<unique_ptr<RDGNode>> _nodes;
    vector<unique_ptr<RDGEdge>> _edges;
};

}  // namespace radray
