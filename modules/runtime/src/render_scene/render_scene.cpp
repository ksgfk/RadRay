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
    for (PrimitiveId id : batch.RemovePrimitives) {
        if (!ContainsPrimitive(id)) RADRAY_ABORT("Invalid primitive removal");
        auto& slot = _primitives[id.Index];
        if (slot.Mesh) {
            const PrimitiveId moved = _staticMeshes.back();
            _staticMeshes[slot.MeshIndex] = moved;
            _primitives[moved.Index].MeshIndex = slot.MeshIndex;
            _staticMeshes.pop_back();
            slot.Mesh.reset();
            slot.MeshIndex = std::numeric_limits<size_t>::max();
        }
        if (slot.Light) {
            const auto moved = _lights.back();
            _lights[slot.LightIndex] = moved;
            _primitives[moved.Index].LightIndex = slot.LightIndex;
            _lights.pop_back();
            slot.Light.reset();
            slot.LightIndex = std::numeric_limits<size_t>::max();
        }
        slot.Alive = false;
        if (slot.Generation == std::numeric_limits<uint32_t>::max()) RADRAY_ABORT("Primitive generation exhausted");
        ++slot.Generation;
    }
    for (PrimitiveId id : batch.CreatePrimitives) {
        if (!id.IsValid()) RADRAY_ABORT("Invalid primitive creation");
        if (id.Index >= _primitives.size()) _primitives.resize(static_cast<size_t>(id.Index) + 1);
        auto& slot = _primitives[id.Index];
        if (slot.Alive || id.Generation < slot.Generation) RADRAY_ABORT("Stale primitive creation");
        slot.Generation = id.Generation;
        slot.Alive = true;
    }
    for (const auto& update : batch.MeshStates) {
        if (!ContainsPrimitive(update.Id)) RADRAY_ABORT("Invalid static mesh state update");
        auto& slot = _primitives[update.Id.Index];
        if (slot.Light) RADRAY_ABORT("Cannot change primitive type");
        if (slot.Mesh) {
            slot.Mesh->Replace(update);
        } else {
            slot.Mesh = make_unique<StaticMeshProxy>(update);
            slot.MeshIndex = _staticMeshes.size();
            _staticMeshes.push_back(update.Id);
        }
    }
    for (const auto& update : batch.Transforms) {
        if (!ContainsPrimitive(update.Id) || !_primitives[update.Id.Index].Mesh) RADRAY_ABORT("Invalid primitive transform update");
        _primitives[update.Id.Index].Mesh->SetTransform(update.LocalToWorld);
    }
    for (const auto& light : batch.Lights) {
        if (!ContainsPrimitive(light.Id)) RADRAY_ABORT("Invalid light update");
        auto& slot = _primitives[light.Id.Index];
        if (slot.Mesh) RADRAY_ABORT("Cannot change primitive type");
        if (!slot.Light) {
            slot.LightIndex = _lights.size();
            _lights.push_back(light.Id);
        }
        slot.Light = light;
    }
}

bool RenderScene::ContainsPrimitive(PrimitiveId id) const noexcept {
    return id.IsValid() && id.Index < _primitives.size() &&
           _primitives[id.Index].Alive && _primitives[id.Index].Generation == id.Generation;
}

std::optional<StaticMeshSceneView> RenderScene::GetStaticMesh(PrimitiveId id) const noexcept {
    if (!ContainsPrimitive(id) || !_primitives[id.Index].Mesh) return std::nullopt;
    return _primitives[id.Index].Mesh->GetView();
}

Nullable<const LightStateUpdate*> RenderScene::GetLight(PrimitiveId id) const noexcept {
    if (!ContainsPrimitive(id) || !_primitives[id.Index].Light) return nullptr;
    return &*_primitives[id.Index].Light;
}

}  // namespace radray
