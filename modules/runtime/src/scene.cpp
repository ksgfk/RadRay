#include <radray/runtime/scene.h>

#include <limits>
#include <algorithm>
#include <cstring>
#include <limits>
#include <tuple>

#include <radray/logger.h>

namespace radray {
namespace {
template <class T>
const T* Lookup(const map<uint64_t, T>& current, const map<uint64_t, T>& changes,
                const vector<uint64_t>& removed, bool reset, uint64_t id) {
    if (auto it = changes.find(id); it != changes.end()) return &it->second;
    if (reset || std::binary_search(removed.begin(), removed.end(), id)) return nullptr;
    auto it = current.find(id);
    return it == current.end() ? nullptr : &it->second;
}
template <class T>
void Publish(map<uint64_t, T>& current, map<uint64_t, T>& changes) noexcept {
    while (!changes.empty()) {
        auto node = changes.extract(changes.begin());
        auto result = current.insert(std::move(node));
        if (!result.inserted) result.position->second = std::move(result.node.mapped());
    }
}
bool Removed(Nullable<const uint64_t*> ids, uint32_t count, vector<uint64_t>& out) {
    if (count) out.assign(ids.Get(), ids.Get() + count);
    std::sort(out.begin(), out.end());
    return (out.empty() || out.front() != 0) && std::adjacent_find(out.begin(), out.end()) == out.end();
}
template <class T>
bool Array(Nullable<const T*> data, uint32_t count, uint32_t max = 1048576) {
    return count <= max && (!count || data);
}
}  // namespace

MeshPassProcessor::MeshPassProcessor() : _identity(make_shared<MeshProcessorIdentity>()) {}
CachedCommands MeshPassProcessor::Build(const PrimitiveUpdate& p, const MeshBatch& mesh,
                                        const MaterialData& material, uint64_t indexVersion, uint64_t textureVersion) const {
    CachedCommands result{p.mesh, p.material, mesh.version, material.version, indexVersion, textureVersion, Version(), Identity(), {}};
    result.commands.reserve(mesh.elements.size());
    for (uint32_t i = 0; i < mesh.elements.size(); ++i)
        result.commands.push_back({p.mesh, p.material, mesh.version, material.version, indexVersion, textureVersion,
                                   material.desc.program, mesh.layout, i, mesh.elements[i], material.desc});
    return result;
}

const MeshBatch& Scene::Mesh(const Update& u, uint64_t id) const {
    return *Lookup(_meshes, u.meshes, u.removedMeshes, u.reset, id);
}
const MaterialData& Scene::Material(const Update& u, uint64_t id) const {
    return *Lookup(_materials, u.materials, u.removedMaterials, u.reset, id);
}
bool Scene::HasMaterial(const Update& u, uint64_t id) const {
    return Lookup(_materials, u.materials, u.removedMaterials, u.reset, id) != nullptr;
}
bool Scene::Prepare(const SceneUpdate& f, const ViewSubmission& view, const MeshPassProcessor& processor, uint64_t textureVersion,
                    Update& u) const {
    if (!view.instanceStride || view.instances.size() % view.instanceStride || !f.epoch || f.epoch < _epoch ||
        !Array(f.indices, f.index_count, 65536) || !Array(f.meshes, f.mesh_count, 65536) ||
        !Array(f.materials, f.material_count, 65536) || !Array(f.primitives, f.primitive_count, 65536) ||
        !Array(f.removed_meshes, f.removed_mesh_count) || !Array(f.removed_primitives, f.removed_primitive_count) ||
        !Array(f.removed_materials, f.removed_material_count) || !Array(f.removed_indices, f.removed_index_count) ||
        view.visible.size() > 1048576 || view.instances.size() / view.instanceStride > 1048576) {
        RADRAY_ERR_LOG("Invalid frame layout, epoch or array");
        return false;
    }
    u.epoch = f.epoch;
    u.reset = f.epoch != _epoch;
    u.revision = _revision;
    if (!Removed(f.removed_meshes, f.removed_mesh_count, u.removedMeshes) ||
        !Removed(f.removed_primitives, f.removed_primitive_count, u.removedPrimitives) ||
        !Removed(f.removed_materials, f.removed_material_count, u.removedMaterials) ||
        !Removed(f.removed_indices, f.removed_index_count, u.removedIndices)) {
        RADRAY_ERR_LOG("Invalid removed IDs");
        return false;
    }
    for (uint32_t i = 0; i < f.index_count; ++i) {
        const auto& x = f.indices.Get()[i];
        if (!x.id || x.reserved || !x.count || !Array(x.indices, x.count, 16u << 20) ||
            std::binary_search(u.removedIndices.begin(), u.removedIndices.end(), x.id)) {
            RADRAY_ERR_LOG("Invalid index resource");
            return false;
        }
        auto data = make_shared<IndexData>();
        data->version = ++u.revision;
        data->values.assign(x.indices.Get(), x.indices.Get() + x.count);
        if (!u.indices.emplace(x.id, std::move(data)).second) {
            RADRAY_ERR_LOG("Duplicate index resource");
            return false;
        }
    }
    for (uint32_t i = 0; i < f.mesh_count; ++i) {
        const auto& x = f.meshes.Get()[i];
        if (!x.id || !x.vertex_count || x.vertex_count > (1u << 20) || !x.submesh_count ||
            !Array(x.submeshes, x.submesh_count, 65536) || x.vertex_stride > 4096 || (x.vertex_stride && !x.vertices) ||
            std::binary_search(u.removedMeshes.begin(), u.removedMeshes.end(), x.id)) {
            RADRAY_ERR_LOG("Invalid mesh layout or data");
            return false;
        }
        MeshBatch mesh{++u.revision, x.index_buffer, x.vertex_count, x.vertex_stride, x.layout, {x.submeshes.Get(), x.submeshes.Get() + x.submesh_count}};
        if (!u.meshes.emplace(x.id, std::move(mesh)).second) {
            RADRAY_ERR_LOG("Duplicate mesh");
            return false;
        }
    }
    for (uint32_t i = 0; i < f.material_count; ++i) {
        const auto& x = f.materials.Get()[i];
        if (!x.id || !x.program ||
            std::binary_search(u.removedMaterials.begin(), u.removedMaterials.end(), x.id)) {
            RADRAY_ERR_LOG("Invalid material");
            return false;
        }
        if (!u.materials.emplace(x.id, MaterialData{x, ++u.revision}).second) {
            RADRAY_ERR_LOG("Duplicate material");
            return false;
        }
    }
    for (uint32_t i = 0; i < f.primitive_count; ++i) {
        const auto& x = f.primitives.Get()[i];
        if (!x.id || !x.mesh || !x.material || std::binary_search(u.removedPrimitives.begin(), u.removedPrimitives.end(), x.id) ||
            !u.primitives.emplace(x.id, x).second) {
            RADRAY_ERR_LOG("Invalid or duplicate primitive");
            return false;
        }
    }
    const auto checkMesh = [&](const MeshBatch& mesh) {
        const auto* indices = mesh.indexBuffer ? Lookup(_indices, u.indices, u.removedIndices, u.reset, mesh.indexBuffer) : nullptr;
        if (mesh.indexBuffer && !indices) return false;
        for (const auto& element : mesh.elements) {
            if (element.reserved || !element.count) return false;
            if (indices) {
                if (uint64_t(element.first) + element.count > (*indices)->values.size()) return false;
                for (uint32_t j = 0; j < element.count; ++j) {
                    const int64_t vertex = int64_t((*indices)->values[element.first + j]) + element.base_vertex;
                    if (vertex < 0 || vertex >= mesh.vertexCount) return false;
                }
            } else if (element.base_vertex || uint64_t(element.first) + element.count > mesh.vertexCount)
                return false;
        }
        return true;
    };
    for (const auto& [id, mesh] : u.meshes)
        if (!checkMesh(mesh)) {
            RADRAY_ERR_LOG("Submesh exceeds geometry bounds");
            return false;
        }
    // An index replacement can change bounds for existing geometry as well.
    if (!u.reset && (!u.indices.empty() || !u.removedIndices.empty()))
        for (const auto& [id, mesh] : _meshes)
            if (!u.meshes.contains(id) && !std::binary_search(u.removedMeshes.begin(), u.removedMeshes.end(), id) && !checkMesh(mesh)) {
                RADRAY_ERR_LOG("Index update leaves an invalid mesh reference");
                return false;
            }
    const auto checkPrimitive = [&](const PrimitiveUpdate& p) {
        const auto* mesh = Lookup(_meshes, u.meshes, u.removedMeshes, u.reset, p.mesh);
        const auto* material = Lookup(_materials, u.materials, u.removedMaterials, u.reset, p.material);
        return mesh && material && processor.Validate(*mesh, material->desc);
    };
    for (const auto& [id, p] : u.primitives)
        if (!checkPrimitive(p)) {
            RADRAY_ERR_LOG("Primitive has invalid mesh or material");
            return false;
        }
    if (!u.reset && (!u.meshes.empty() || !u.materials.empty() || !u.removedMeshes.empty() || !u.removedMaterials.empty()))
        for (const auto& [id, p] : _primitives)
            if (!u.primitives.contains(id) && !std::binary_search(u.removedPrimitives.begin(), u.removedPrimitives.end(), id) && !checkPrimitive(p)) {
                RADRAY_ERR_LOG("Scene update leaves an invalid primitive reference");
                return false;
            }
    u.visible.reserve(view.visible.size());
    for (const auto& item : view.visible) {
        const auto* p = Lookup(_primitives, u.primitives, u.removedPrimitives, u.reset, item.primitive);
        if (!p || !checkPrimitive(*p)) {
            RADRAY_ERR_LOG("Visible item references a missing primitive");
            return false;
        }
        const auto& mesh = Mesh(u, p->mesh);
        const auto& material = Material(u, p->material);
        if (item.submesh >= mesh.elements.size() || !item.instance_count ||
            uint64_t(item.first_instance) + item.instance_count > view.instances.size() / view.instanceStride) {
            RADRAY_ERR_LOG("Invalid visible range");
            return false;
        }
        const uint64_t iv = mesh.indexBuffer ? (*Lookup(_indices, u.indices, u.removedIndices, u.reset, mesh.indexBuffer))->version : 0;
        const uint64_t tv = material.desc.resources.empty() ? 0 : textureVersion;
        shared_ptr<const CachedCommands> cached;
        if (auto it = u.commands.find(p->id); it != u.commands.end())
            cached = it->second;
        else if (!u.reset)
            if (auto old = _commands.find(p->id); old != _commands.end()) cached = old->second;
        if (!cached || cached->mesh != p->mesh || cached->material != p->material || cached->meshVersion != mesh.version ||
            cached->materialVersion != material.version || cached->indexVersion != iv || cached->textureVersion != tv || cached->processorVersion != processor.Version() || cached->processorIdentity != processor.Identity()) {
            cached = make_shared<CachedCommands>(processor.Build(*p, mesh, material, iv, tv));
            u.commands.insert_or_assign(p->id, cached);
            ++u.builds;
        }
        u.visible.push_back({std::move(cached), item});
    }
    return true;
}
void Scene::Commit(Update&& u) noexcept {
    if (u.reset) {
        _indices.clear();
        _meshes.clear();
        _materials.clear();
        _primitives.clear();
        _commands.clear();
    }
    for (auto id : u.removedIndices) _indices.erase(id);
    for (auto id : u.removedMeshes) _meshes.erase(id);
    for (auto id : u.removedMaterials) _materials.erase(id);
    for (auto id : u.removedPrimitives) {
        _primitives.erase(id);
        _commands.erase(id);
    }
    Publish(_indices, u.indices);
    Publish(_meshes, u.meshes);
    Publish(_materials, u.materials);
    Publish(_primitives, u.primitives);
    Publish(_commands, u.commands);
    _epoch = u.epoch;
    _revision = u.revision;
    _builds += u.builds;
}

bool BuildDrawList(vector<ResolvedDraw> visible, std::span<const byte> source, uint32_t stride, DrawList& output) {
    if (!stride || source.size() % stride) {
        RADRAY_ERR_LOG("Invalid instance stream stride");
        return false;
    }
    for (const auto& draw : visible)
        if (!draw.command || !draw.instanceCount || uint64_t(draw.firstInstance) + draw.instanceCount > source.size() / stride) {
            RADRAY_ERR_LOG("Resolved draw exceeds instance stream");
            return false;
        }
    std::stable_sort(visible.begin(), visible.end(), [](const auto& a, const auto& b) {
        return std::tuple{a.depth, a.command->program, a.command->material, a.page, a.command->mesh, a.firstInstance, a.command->range.first} <
               std::tuple{b.depth, b.command->program, b.command->material, b.page, b.command->mesh, b.firstInstance, b.command->range.first};
    });
    DrawList result;
    uint32_t previousSource = std::numeric_limits<uint32_t>::max(), previousCount = 0, instanceOffset = 0;
    for (const auto& v : visible) {
        const auto& c = *v.command;
        if (previousSource != v.firstInstance || previousCount != v.instanceCount) {
            instanceOffset = static_cast<uint32_t>(result.instances.size() / stride);
            result.instances.insert(result.instances.end(), source.begin() + size_t(v.firstInstance) * stride, source.begin() + size_t(v.firstInstance + v.instanceCount) * stride);
            previousSource = v.firstInstance;
            previousCount = v.instanceCount;
        }
        DrawPacket packet{c.mesh, c.material, v.page, v.indexBuffer, c.program, c.range.count, v.instanceCount, c.range.first, instanceOffset, v.baseVertex + c.range.base_vertex, 0, {c.meshVersion, c.materialVersion, c.indexVersion, c.textureVersion, c.layout}};
        if (!result.packets.empty()) {
            auto& last = result.packets.back();
            const bool state = last.mesh == packet.mesh && last.material == packet.material && last.program == packet.program && last.page == packet.page &&
                               last.indexBuffer == packet.indexBuffer && last.baseVertex == packet.baseVertex && last.state == packet.state;
            if (state && last.firstInstance == packet.firstInstance && last.instances == packet.instances && last.first + last.count == packet.first) {
                last.count += packet.count;
                continue;
            }
            if (state && last.first == packet.first && last.count == packet.count && last.firstInstance + last.instances == packet.firstInstance) {
                last.instances += packet.instances;
                continue;
            }
        }
        result.packets.push_back(packet);
    }
    for (auto& p : result.packets) {
        p.indirectOffset = result.indirect.size();
        const uint32_t indexed[]{p.count, p.instances, p.first, uint32_t(p.baseVertex), p.firstInstance};
        const uint32_t direct[]{p.count, p.instances, p.first, p.firstInstance};
        const auto* data = reinterpret_cast<const byte*>(p.indexBuffer ? indexed : direct);
        result.indirect.insert(result.indirect.end(), data, data + (p.indexBuffer ? sizeof(indexed) : sizeof(direct)));
    }
    output = std::move(result);
    return true;
}
}  // namespace radray
