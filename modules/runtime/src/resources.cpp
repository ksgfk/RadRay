#include <radray/runtime/resources.h>

#include <algorithm>
#include <cstring>

#include <radray/logger.h>
#include <radray/runtime/graph_execution.h>

namespace radray {
Nullable<RenderBuffer> CreateBuffer(render::Device& d, uint64_t size, render::MemoryType memory, render::BufferUses usage) {
    if (memory == render::MemoryType::Upload) usage |= render::BufferUse::MapWrite;
    return d.CreateBuffer({size, memory, usage, memory == render::MemoryType::Upload ? render::ResourceHint::PersistentMap : render::ResourceHint::None}).Release();
}
bool WriteBuffer(render::Buffer& buffer, Nullable<const void*> data, size_t bytes) {
    if (!bytes) return true;
    auto* mapped = buffer.Map(0, bytes);
    if (!mapped) return false;
    std::memcpy(mapped, data.Get(), bytes);
    buffer.FlushMappedRange({0, bytes});
    buffer.Unmap();
    return true;
}
GeometryAllocation::GeometryAllocation(shared_ptr<GeometryPage> p, uint32_t offset, uint32_t n)
    : page(std::move(p)), first(offset), count(0) {
    map<uint32_t, uint32_t> range{{offset, n}};
    returnRange = range.extract(range.begin());
}

GeometryAllocation::~GeometryAllocation() {
    if (!count) return;
    auto& free = page->free;
    auto it = free.insert(std::move(returnRange)).position;
    if (it != free.begin()) {
        auto previous = std::prev(it);
        if (previous->first + previous->second == it->first) {
            previous->second += it->second;
            free.erase(it);
            it = previous;
        }
    }
    auto next = std::next(it);
    if (next != free.end() && it->first + it->second == next->first) {
        it->second += next->second;
        free.erase(next);
    }
}
GeometryStore::GeometryStore(DeviceContext& runtime) : _runtime(runtime) {}

shared_ptr<GeometryAllocation> GeometryStore::Allocate(uint32_t count, uint32_t stride) {
    for (auto& page : _pages)
        if (page->stride == stride)
            for (auto it = page->free.begin(); it != page->free.end(); ++it)
                if (it->second >= count) {
                    const auto [first, length] = *it;
                    auto allocation = make_shared<GeometryAllocation>(page, first, count);
                    if (length > count) page->free.emplace(first + count, length - count);
                    page->free.erase(it);
                    allocation->count = count;
                    return allocation;
                }
    const auto capacity = std::max((4u * 1024 * 1024 / stride / 4) * 4, count);
    auto buffer = CreateBuffer(_runtime.GetDevice(), uint64_t(capacity) * stride, render::MemoryType::Device, render::BufferUse::CopyDestination | render::BufferUse::Vertex).Release();
    if (!buffer) return {};
    auto page = make_shared<GeometryPage>(GeometryPage{std::move(buffer), stride, ++_nextPage, {}, false});
    if (capacity > count) page->free.emplace(count, capacity - count);
    auto allocation = make_shared<GeometryAllocation>(page, 0, count);
    _pages.push_back(page);
    allocation->count = count;
    return allocation;
}
bool GeometryStore::Prepare(const SceneUpdate& f, Update& u, RenderBuffer& staging, uint64_t& stagingSize) {
    std::erase_if(_pages, [](const auto& page) { return page.use_count() == 1; });
    uint64_t bytes = 0;
    for (uint32_t i = 0; i < f.index_count; ++i) bytes += uint64_t(f.indices.Get()[i].count) * 4;
    for (uint32_t i = 0; i < f.mesh_count; ++i) bytes += uint64_t(f.meshes.Get()[i].vertex_count) * f.meshes.Get()[i].vertex_stride;
    if (bytes > stagingSize) {
        auto replacement = CreateBuffer(_runtime.GetDevice(), bytes, render::MemoryType::Upload, render::BufferUse::CopySource).Release();
        if (!replacement) {
            RADRAY_ERR_LOG("Cannot allocate upload staging");
            return false;
        }
        staging = std::move(replacement);
        stagingSize = bytes;
    }
    struct Mapping {
        render::Buffer* buffer;
        byte* data;
        ~Mapping() {
            if (data) buffer->Unmap();
        }
    } mapping{staging.get(), bytes ? static_cast<byte*>(staging->Map(0, bytes)) : nullptr};
    if (bytes && !mapping.data) {
        RADRAY_ERR_LOG("Cannot map upload staging");
        return false;
    }
    uint64_t offset = 0;
    for (uint32_t i = 0; i < f.index_count; ++i) {
        const auto& x = f.indices.Get()[i];
        const uint64_t size = uint64_t(x.count) * 4;
        auto buffer = CreateBuffer(_runtime.GetDevice(), size, render::MemoryType::Device, render::BufferUse::CopyDestination | render::BufferUse::Index).Release();
        if (!buffer) {
            RADRAY_ERR_LOG("Cannot allocate index resource");
            return false;
        }
        auto allocation = make_shared<IndexAllocation>(IndexAllocation{std::move(buffer)});
        u.uploads.push_back({allocation->buffer.get(), 0, offset, size, {}});
        std::memcpy(mapping.data + offset, x.indices.Get(), static_cast<size_t>(size));
        offset += size;
        u.indices.emplace(x.id, allocation);
        u.keepAlive.push_back(std::move(allocation));
    }
    set<uint64_t> touched;
    for (uint32_t i = 0; i < f.mesh_count; ++i) {
        const auto& x = f.meshes.Get()[i];
        if (!x.vertex_stride) {
            u.meshes.emplace(x.id, nullptr);
            continue;
        }
        auto allocation = Allocate(x.vertex_count, x.vertex_stride);
        if (!allocation) {
            RADRAY_ERR_LOG("Cannot allocate geometry page");
            return false;
        }
        const uint64_t size = uint64_t(x.vertex_count) * x.vertex_stride;
        u.uploads.push_back({allocation->page->buffer.get(), uint64_t(allocation->first) * x.vertex_stride, offset, size, allocation->page});
        std::memcpy(mapping.data + offset, x.vertices.Get(), static_cast<size_t>(size));
        offset += size;
        if (touched.insert(allocation->page->id).second) u.touched.push_back(allocation->page);
        u.meshes.emplace(x.id, allocation);
        u.keepAlive.push_back(std::move(allocation));
    }
    if (bytes) staging->FlushMappedRange({0, bytes});
    return true;
}
shared_ptr<GeometryAllocation> GeometryStore::Mesh(const Update& u, uint64_t id) const {
    if (auto it = u.meshes.find(id); it != u.meshes.end()) return it->second;
    return _meshes.at(id);
}
shared_ptr<IndexAllocation> GeometryStore::Index(const Update& u, uint64_t id) const {
    if (auto it = u.indices.find(id); it != u.indices.end()) return it->second;
    return _indices.at(id);
}
void GeometryStore::AddUploadPass(GraphBuilder& graph, const Update& u, render::Buffer* staging) {
    if (u.uploads.empty()) return;
    const auto src = graph.ImportBuffer("Scene staging", staging, {ResourceState::Buffer(render::BufferState::CopySource), ResourceState::Buffer(render::BufferState::CopySource)});
    vector<ResourceAccess> access{{src, Access::CopySource}};
    struct Copy {
        BufferHandle target;
        uint64_t dst, src, size;
    };
    vector<Copy> copies;
    for (const auto& upload : u.uploads) {
        const auto final = ResourceState::Buffer(upload.page ? render::BufferState::Vertex : render::BufferState::Index);
        const auto initial = upload.page && upload.page->initialized ? final : ResourceState{};
        auto dst = graph.ImportBuffer("Geometry", upload.target, {initial, final, upload.page && upload.page->initialized});
        access.push_back({dst, Access::CopyDestination});
        copies.push_back({dst, upload.targetOffset, upload.sourceOffset, upload.bytes});
    }
    graph.AddCopyPass("SceneUpload", std::move(access), [src, copies = std::move(copies)](PassContext& c) {
        for (const auto& copy : copies) c.Copy()->CopyBufferToBuffer(c.Buffer(copy.target).Get(), copy.dst, c.Buffer(src).Get(), copy.src, copy.size);
        return true;
    });
}
void GeometryStore::Commit(Update& u, const Scene::Update& scene) noexcept {
    if (scene.reset) {
        _meshes.clear();
        _indices.clear();
    }
    for (auto id : scene.removedMeshes) _meshes.erase(id);
    for (auto id : scene.removedIndices) _indices.erase(id);
    const auto publish = [](auto& target, auto& changes) {
        while (!changes.empty()) {
            auto result = target.insert(changes.extract(changes.begin()));
            if (!result.inserted) result.position->second = std::move(result.node.mapped());
        }
    };
    publish(_meshes, u.meshes);
    publish(_indices, u.indices);
    for (auto& page : u.touched) page->initialized = true;
}
uint64_t GeometryStore::ResidentBytes() const {
    uint64_t size = 0;
    for (auto& page : _pages) size += page->buffer->GetDesc().Size;
    return size;
}
}  // namespace radray
