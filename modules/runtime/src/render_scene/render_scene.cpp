#include <radray/runtime/render_scene/render_scene.h>

#include <mutex>
#include <utility>

#include <radray/logger.h>
#include <radray/profiler.h>

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
    RADRAY_PROFILE_SCOPE_N("RenderScene::Apply");
    std::unique_lock lock{_readers};
    _readersDone.wait(lock, [this] { return _activeReaders == 0; });
    for (ShapeId id : batch.RemoveShapes) {
        if (!ContainsShape(id)) RADRAY_ABORT("Invalid shape removal");
        auto& slot = _shapes[id.Index];
        if (slot.MeshIndex != kNoMesh) {
            const uint32_t removed = slot.MeshIndex;
            const auto ids = _staticMeshes.GetColumns().Ids;
            const uint32_t last = static_cast<uint32_t>(ids.size() - 1);
            if (removed != last) {
                const ShapeId moved = ids[last];
                _shapes[moved.Index].MeshIndex = removed;
            }
            _staticMeshes.Remove(removed);
            slot.MeshIndex = kNoMesh;
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
        if (slot.MeshIndex != kNoMesh) {
            _staticMeshes.Replace(slot.MeshIndex, update);
        } else {
            slot.MeshIndex = static_cast<uint32_t>(_staticMeshes.GetColumns().Size());
            _staticMeshes.Add(update);
        }
    }
    for (const auto& update : batch.Transforms) {
        if (!ContainsShape(update.Id) || _shapes[update.Id.Index].MeshIndex == kNoMesh) RADRAY_ABORT("Invalid shape transform update");
        _staticMeshes.SetTransform(_shapes[update.Id.Index].MeshIndex, update.LocalToWorld);
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
    if (!ContainsShape(id) || _shapes[id.Index].MeshIndex == kNoMesh) return std::nullopt;
    return _staticMeshes.GetColumns().Get(_shapes[id.Index].MeshIndex);
}
}  // namespace radray
