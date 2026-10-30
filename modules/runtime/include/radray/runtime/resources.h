#pragma once

#include <radray/runtime/device.h>
#include <radray/runtime/scene.h>
#include <radray/runtime/graph.h>

namespace radray {

using RenderBuffer = unique_ptr<render::Buffer>;

Nullable<RenderBuffer> CreateBuffer(render::Device&, uint64_t, render::MemoryType, render::BufferUses);

bool WriteBuffer(render::Buffer&, Nullable<const void*>, size_t);

struct GeometryPage {
    RenderBuffer buffer;
    uint32_t stride;
    uint64_t id;
    map<uint32_t, uint32_t> free;
    bool initialized{};
};

struct GeometryAllocation {
    shared_ptr<GeometryPage> page;
    uint32_t first, count;
    map<uint32_t, uint32_t>::node_type returnRange;
    GeometryAllocation(shared_ptr<GeometryPage> p, uint32_t offset, uint32_t n);
    ~GeometryAllocation();
};

struct IndexAllocation {
    RenderBuffer buffer;
};

class GeometryStore {
public:
    struct Upload {
        render::Buffer* target;
        uint64_t targetOffset, sourceOffset, bytes;
        shared_ptr<GeometryPage> page;
    };
    struct Update {
        map<uint64_t, shared_ptr<GeometryAllocation>> meshes;
        map<uint64_t, shared_ptr<IndexAllocation>> indices;
        vector<Upload> uploads;
        vector<shared_ptr<void>> keepAlive;
        vector<shared_ptr<GeometryPage>> touched;
    };
    explicit GeometryStore(DeviceContext& runtime);
    bool Prepare(const SceneUpdate&, Update&, RenderBuffer& staging, uint64_t& stagingSize);
    shared_ptr<GeometryAllocation> Mesh(const Update&, uint64_t) const;
    shared_ptr<IndexAllocation> Index(const Update&, uint64_t) const;
    void AddUploadPass(GraphBuilder&, const Update&, render::Buffer* staging);
    void Commit(Update&, const Scene::Update&) noexcept;
    uint64_t ResidentBytes() const;

private:
    shared_ptr<GeometryAllocation> Allocate(uint32_t count, uint32_t stride);
    DeviceContext& _runtime;
    vector<shared_ptr<GeometryPage>> _pages;
    uint64_t _nextPage{};
    map<uint64_t, shared_ptr<GeometryAllocation>> _meshes;
    map<uint64_t, shared_ptr<IndexAllocation>> _indices;
};

}  // namespace radray
