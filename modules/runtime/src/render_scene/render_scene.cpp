#include <radray/runtime/render_scene/render_scene.h>

#include "static_mesh_proxy.h"

#include <radray/logger.h>
#include <mutex>

namespace radray {

RenderScene::RenderScene() noexcept = default;
RenderScene::~RenderScene() noexcept {
    std::unique_lock lock{_readers};
    _readersDone.wait(lock, [this] { return _activeReaders == 0; });
}

RenderScene::ReadLease::ReadLease(const RenderScene* scene) noexcept : _scene(scene) {}
RenderScene::ReadLease::ReadLease(ReadLease&& other) noexcept : _scene(std::exchange(other._scene, nullptr)) {}
RenderScene::ReadLease& RenderScene::ReadLease::operator=(ReadLease&& other) noexcept {
    if (this != &other) {
        Release();
        _scene = std::exchange(other._scene, nullptr);
    }
    return *this;
}
RenderScene::ReadLease::~ReadLease() noexcept { Release(); }
void RenderScene::ReadLease::Release() noexcept {
    if (!_scene) return;
    auto scene = std::exchange(_scene, nullptr);
    std::lock_guard lock{scene->_readers};
    if (--scene->_activeReaders == 0) scene->_readersDone.notify_all();
}
RenderScene::ReadLease RenderScene::AcquireRead() const {
    std::lock_guard lock{_readers};
    ++_activeReaders;
    return ReadLease{this};
}

void RenderScene::Apply(const SceneUpdateBatch& batch) noexcept {
    std::unique_lock lock{_readers};
    _readersDone.wait(lock, [this] { return _activeReaders == 0; });
    for (ShapeId id : batch.RemoveShapes) {
        if (!ContainsShape(id)) RADRAY_ABORT("Invalid shape removal");
        auto& slot = _shapes[id.Index];
        if (slot.Mesh) {
            const ShapeId moved = _staticMeshes.back();
            _staticMeshes[slot.MeshIndex] = moved;
            _shapes[moved.Index].MeshIndex = slot.MeshIndex;
            _staticMeshes.pop_back();
            slot.Mesh.reset();
            slot.MeshIndex = std::numeric_limits<size_t>::max();
        }
        slot.Alive = false;
        if (slot.Generation == std::numeric_limits<uint32_t>::max()) RADRAY_ABORT("Shape generation exhausted");
        ++slot.Generation;
    }
    for (ShapeId id : batch.CreateShapes) {
        if (!id.IsValid()) RADRAY_ABORT("Invalid shape creation");
        if (id.Index >= _shapes.size()) _shapes.resize(static_cast<size_t>(id.Index) + 1);
        auto& slot = _shapes[id.Index];
        if (slot.Alive || id.Generation < slot.Generation) RADRAY_ABORT("Stale shape creation");
        slot.Generation = id.Generation;
        slot.Alive = true;
    }
    for (const auto& update : batch.MeshStates) {
        if (!ContainsShape(update.Id)) RADRAY_ABORT("Invalid static mesh state update");
        auto& slot = _shapes[update.Id.Index];
        if (slot.Mesh) {
            slot.Mesh->Replace(update);
        } else {
            slot.Mesh = make_unique<StaticMeshProxy>(update);
            slot.MeshIndex = _staticMeshes.size();
            _staticMeshes.push_back(update.Id);
        }
    }
    for (const auto& update : batch.Transforms) {
        if (!ContainsShape(update.Id) || !_shapes[update.Id.Index].Mesh) RADRAY_ABORT("Invalid shape transform update");
        _shapes[update.Id.Index].Mesh->SetTransform(update.LocalToWorld);
    }
    if (batch.LightsChanged)
        _lights.Assign(batch.Lights);
    else if (!batch.Lights.Empty())
        RADRAY_ABORT("Light snapshot requires LightsChanged");
}

bool RenderScene::ContainsShape(ShapeId id) const noexcept {
    return id.IsValid() && id.Index < _shapes.size() &&
           _shapes[id.Index].Alive && _shapes[id.Index].Generation == id.Generation;
}

std::optional<StaticMeshSceneView> RenderScene::GetStaticMesh(ShapeId id) const noexcept {
    if (!ContainsShape(id) || !_shapes[id.Index].Mesh) return std::nullopt;
    return _shapes[id.Index].Mesh->GetView();
}
}  // namespace radray
