#include <radray/runtime/render_framework/scene.h>

#include "static_mesh_proxy.h"

#include <radray/logger.h>

namespace radray {

void SceneUpdateBatch::Clear() noexcept {
    RemovePrimitives.clear();
    CreatePrimitives.clear();
    MeshStates.clear();
    Transforms.clear();
}

Scene::Scene() noexcept = default;
Scene::~Scene() noexcept = default;
Scene::Scene(Scene&&) noexcept = default;
Scene& Scene::operator=(Scene&&) noexcept = default;

void Scene::Apply(const SceneUpdateBatch& batch) noexcept {
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
        slot.Alive = false;
    }
    for (PrimitiveId id : batch.CreatePrimitives) {
        if (!id.IsValid()) RADRAY_ABORT("Invalid primitive creation");
        if (id.Index >= _primitives.size()) _primitives.resize(static_cast<size_t>(id.Index) + 1);
        auto& slot = _primitives[id.Index];
        if (slot.Alive || id.Generation <= slot.Generation) RADRAY_ABORT("Stale primitive creation");
        slot.Generation = id.Generation;
        slot.Alive = true;
    }
    for (const auto& update : batch.MeshStates) {
        if (!ContainsPrimitive(update.Id)) RADRAY_ABORT("Invalid static mesh state update");
        auto& slot = _primitives[update.Id.Index];
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
}

bool Scene::ContainsPrimitive(PrimitiveId id) const noexcept {
    return id.IsValid() && id.Index < _primitives.size() &&
           _primitives[id.Index].Alive && _primitives[id.Index].Generation == id.Generation;
}

std::optional<StaticMeshSceneView> Scene::GetStaticMesh(PrimitiveId id) const noexcept {
    if (!ContainsPrimitive(id) || !_primitives[id.Index].Mesh) return std::nullopt;
    return _primitives[id.Index].Mesh->GetView();
}

}  // namespace radray
