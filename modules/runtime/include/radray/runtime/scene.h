#pragma once

#include <span>

#include <radray/types.h>
#include <radray/nullable.h>

namespace radray {

struct DrawRange {
    uint32_t first{}, count{};
    int32_t base_vertex{};
    uint32_t reserved{};
};

struct IndexUpdate {
    uint64_t id{};
    Nullable<const uint32_t*> indices{};
    uint32_t count{}, reserved{};
};

struct MeshUpdate {
    uint64_t id{};
    Nullable<const void*> vertices{};
    uint64_t index_buffer{};
    Nullable<const DrawRange*> submeshes{};
    uint32_t vertex_count{}, vertex_stride{}, layout{}, submesh_count{};
};

struct MaterialDescriptor {
    uint64_t id{};
    uint32_t program{};
    vector<uint32_t> resources;
    vector<byte> parameters;
};

struct PrimitiveUpdate {
    uint64_t id{}, mesh{}, material{};
};

struct VisibleItem {
    uint64_t primitive{};
    uint32_t submesh{}, depth_key{}, first_instance{}, instance_count{};
};

struct SceneUpdate {
    uint64_t epoch{};
    Nullable<const IndexUpdate*> indices{};
    Nullable<const MeshUpdate*> meshes{};
    Nullable<const MaterialDescriptor*> materials{};
    Nullable<const PrimitiveUpdate*> primitives{};
    Nullable<const uint64_t*> removed_indices{};
    Nullable<const uint64_t*> removed_meshes{};
    Nullable<const uint64_t*> removed_materials{};
    Nullable<const uint64_t*> removed_primitives{};
    uint32_t index_count{}, mesh_count{}, material_count{}, primitive_count{}, removed_index_count{}, removed_mesh_count{}, removed_material_count{}, removed_primitive_count{};
};

struct ViewSubmission {
    std::span<const VisibleItem> visible;
    std::span<const byte> instances;
    uint32_t instanceStride{};
};

// See docs/architecture/runtime.md for scene, command and frame lifetimes.
struct IndexData {
    uint64_t version{};
    vector<uint32_t> values;
};

struct MeshBatch {
    uint64_t version{}, indexBuffer{};
    uint32_t vertexCount{}, stride{}, layout{};
    vector<DrawRange> elements;
};

struct MaterialData {
    MaterialDescriptor desc{};
    uint64_t version{};
};

struct MeshDrawCommand {
    uint64_t mesh{}, material{}, meshVersion{}, materialVersion{}, indexVersion{}, textureVersion{};
    uint32_t program{}, layout{}, element{};
    DrawRange range{};
    MaterialDescriptor bindings{};  // Constants resolve to the current frame; texture indices are logical references.
};

struct MeshProcessorIdentity {};

struct CachedCommands {
    uint64_t mesh{}, material{}, meshVersion{}, materialVersion{}, indexVersion{}, textureVersion{}, processorVersion{};
    shared_ptr<const MeshProcessorIdentity> processorIdentity;
    vector<MeshDrawCommand> commands;
};

struct VisibleMeshDraw {
    shared_ptr<const CachedCommands> cached;
    VisibleItem item{};
    const MeshDrawCommand& Command() const { return cached->commands[item.submesh]; }
};

class MeshPassProcessor {
public:
    MeshPassProcessor();
    MeshPassProcessor(const MeshPassProcessor&) = delete;
    MeshPassProcessor& operator=(const MeshPassProcessor&) = delete;
    virtual ~MeshPassProcessor() = default;
    const shared_ptr<const MeshProcessorIdentity>& Identity() const { return _identity; }
    virtual uint64_t Version() const { return 1; }
    virtual bool Validate(const MeshBatch&, const MaterialDescriptor&) const = 0;
    virtual CachedCommands Build(const PrimitiveUpdate&, const MeshBatch&, const MaterialData&,
                                 uint64_t indexVersion, uint64_t textureVersion) const;

private:
    shared_ptr<const MeshProcessorIdentity> _identity;
};

class Scene {
public:
    struct Update {
        uint64_t epoch{}, revision{}, builds{};
        bool reset{};
        map<uint64_t, shared_ptr<const IndexData>> indices;
        map<uint64_t, MeshBatch> meshes;
        map<uint64_t, MaterialData> materials;
        map<uint64_t, PrimitiveUpdate> primitives;
        map<uint64_t, shared_ptr<const CachedCommands>> commands;
        vector<uint64_t> removedIndices, removedMeshes, removedMaterials, removedPrimitives;
        vector<VisibleMeshDraw> visible;
    };
    bool Prepare(const SceneUpdate&, const ViewSubmission&, const MeshPassProcessor&, uint64_t textureVersion,
                 Update&) const;
    void Commit(Update&&) noexcept;
    const MeshBatch& Mesh(const Update&, uint64_t id) const;
    const MaterialData& Material(const Update&, uint64_t id) const;
    bool HasMaterial(const Update&, uint64_t id) const;
    uint64_t CommandBuilds() const noexcept { return _builds; }

private:
    uint64_t _epoch{}, _revision{}, _builds{};
    map<uint64_t, shared_ptr<const IndexData>> _indices;
    map<uint64_t, MeshBatch> _meshes;
    map<uint64_t, MaterialData> _materials;
    map<uint64_t, PrimitiveUpdate> _primitives;
    map<uint64_t, shared_ptr<const CachedCommands>> _commands;
};

struct ResolvedDraw {
    Nullable<const MeshDrawCommand*> command{};
    uint64_t page{}, indexBuffer{};
    int32_t baseVertex{};
    uint32_t depth{}, firstInstance{}, instanceCount{};
};

struct DrawPacket {
    uint64_t mesh{}, material{}, page{}, indexBuffer{};
    uint32_t program{}, count{}, instances{}, first{}, firstInstance{};
    int32_t baseVertex{};
    uint64_t indirectOffset{};
    array<uint64_t, 5> state{};
};

struct DrawList {
    vector<byte> instances;
    vector<DrawPacket> packets;
    vector<byte> indirect;
};

// Both submission modes consume the same packets; merging never changes geometry identity.
bool BuildDrawList(vector<ResolvedDraw>, std::span<const byte>, uint32_t instanceStride, DrawList&);

}  // namespace radray
