#pragma once

#include <limits>
#include <functional>
#include <optional>

#include <radray/render/rhi.h>

namespace radray {

struct GraphIdentity {};

struct ResourceHandle {
    shared_ptr<const GraphIdentity> graph;
    uint32_t index{std::numeric_limits<uint32_t>::max()};
    friend bool operator==(const ResourceHandle&, const ResourceHandle&) = default;
};

using TextureHandle = ResourceHandle;
using BufferHandle = ResourceHandle;

enum class ResourceKind {
    Buffer,
    Texture
};

enum class PassKind {
    Raster,
    Compute,
    Copy
};

enum class Access {
    CopySource,
    CopyDestination,
    Vertex,
    Index,
    Constant,
    Indirect,
    Sample,
    StorageRead,
    StorageWrite,
    StorageReadWrite,
    Color,
    DepthRead,
    DepthWrite
};

struct ResourceAccess {
    ResourceHandle resource;
    Access access;
    render::ShaderStages stages{};
};

struct ResourceState {
    render::BufferStates buffer{render::BufferState::Undefined};
    render::TextureStates texture{render::TextureState::Undefined};
    render::ShaderStages stages{};
    static ResourceState Buffer(render::BufferStates s, render::ShaderStages stages = {});
    static ResourceState Texture(render::TextureStates s, render::ShaderStages stages = {});
    friend bool operator==(const ResourceState&, const ResourceState&) = default;
};

struct ImportOptions {
    ResourceState initial, final;
    bool initialized{true}, acquire{}, release{};
    shared_ptr<void> owner;
};

struct ColorAttachment {
    TextureHandle texture;
    render::LoadAction load{render::LoadAction::Clear};
    render::StoreAction store{render::StoreAction::Store};
    render::ColorClearValue clear{};
};

struct DepthAttachment {
    TextureHandle texture;
    render::LoadAction load{render::LoadAction::Clear};
    render::StoreAction store{render::StoreAction::Store};
    float clear{};
    bool readOnly{};
};

struct RasterAttachments {
    vector<ColorAttachment> colors;
    std::optional<DepthAttachment> depth;
    uint32_t width{}, height{};
};

struct GraphResource {
    string name;
    ResourceKind kind;
    render::BufferDescriptor buffer{};
    render::TextureDescriptor texture{};
    Nullable<render::Buffer*> importedBuffer{};
    Nullable<render::Texture*> importedTexture{};
    ImportOptions import;
    bool imported{}, output{}, extracted{};
};

class PassContext;

using PassCallback = std::function<bool(PassContext&)>;

struct GraphPass {
    string name;
    PassKind kind;
    vector<ResourceAccess> accesses;
    RasterAttachments attachments;
    PassCallback execute;
    bool sideEffect{};
};

struct PlannedBarrier {
    uint32_t resource;
    ResourceState before, after;
    bool uav{};
    render::ExternalOwnershipTransfer transfer{render::ExternalOwnershipTransfer::None};
};

struct PlannedPass {
    uint32_t index;
    vector<uint32_t> dependencies;
    vector<PlannedBarrier> barriers;
};

struct ResourceLifetime {
    int32_t first{-1}, last{-1};
};

class CompiledGraph {
public:
    shared_ptr<const GraphIdentity> id;
    vector<GraphResource> resources;
    vector<GraphPass> passes;
    vector<PlannedPass> plan;
    vector<ResourceLifetime> lifetimes;
    vector<PlannedBarrier> epilogue;
    vector<ResourceState> finalStates;
    string Dump() const;
};

class PreparedMeshPass;

class GraphBuilder {
public:
    GraphBuilder();
    GraphBuilder(const GraphBuilder&) = delete;
    GraphBuilder& operator=(const GraphBuilder&) = delete;
    TextureHandle CreateTexture(string name, const render::TextureDescriptor&);
    BufferHandle CreateBuffer(string name, const render::BufferDescriptor&);
    TextureHandle ImportTexture(string name, Nullable<render::Texture*>, ImportOptions);
    BufferHandle ImportBuffer(string name, Nullable<render::Buffer*>, ImportOptions);
    std::optional<ResourceHandle> Find(Nullable<const render::Resource*>) const;
    void MarkOutput(ResourceHandle);
    void Extract(ResourceHandle);
    uint32_t AddRasterPass(string name, vector<ResourceAccess>, RasterAttachments, PassCallback, bool sideEffect = false);
    uint32_t AddComputePass(string name, vector<ResourceAccess>, PassCallback, bool sideEffect = false);
    uint32_t AddCopyPass(string name, vector<ResourceAccess>, PassCallback, bool sideEffect = false);
    uint32_t AddMeshPass(string name, PreparedMeshPass&, RasterAttachments);
    bool Compile(CompiledGraph&) const;
    const shared_ptr<const GraphIdentity>& Id() const { return _id; }

private:
    ResourceHandle AddResource(GraphResource);
    uint32_t AddPass(GraphPass);
    shared_ptr<const GraphIdentity> _id;
    vector<GraphResource> _resources;
    vector<GraphPass> _passes;
    bool _failed{};
};

}  // namespace radray
