#include <radray/runtime/components/static_mesh_component.h>

#include <utility>
#include <algorithm>

#include <radray/runtime/game_framework/world.h>

namespace radray {

StaticMeshComponent::~StaticMeshComponent() noexcept = default;

void StaticMeshComponent::SetStaticMesh(StreamingAssetRef<StaticMesh> mesh) {
    CheckCanModify();
    if (_mesh == mesh) {
        return;
    }
    _readyWait.reset();
    _mesh = std::move(mesh);
    MarkRenderStateDirty();
    StartMeshReadyWait();
}

void StaticMeshComponent::SetMaterials(vector<StreamingAssetRef<Material>> materials) {
    CheckCanModify();
    if (_materials == materials) return;
    _materialReadyWait.reset();
    _materials = std::move(materials);
    MarkRenderDynamicDataDirty();
    StartMaterialReadyWaits();
}

void StaticMeshComponent::OnRenderStateCreated() {
    StartMeshReadyWait();
    StartMaterialReadyWaits();
}

void StaticMeshComponent::OnRenderStateDestroyed() {
    _readyWait.reset();
    _materialReadyWait.reset();
}

void StaticMeshComponent::StartMeshReadyWait() {
    if (GetShapeId().IsValid() && _mesh.IsValid() && !_mesh.IsCompleted()) {
        _readyWait = make_unique<TaskScope>();
        _readyWait->Spawn(WaitForMeshReady(_mesh, GetRenderSceneId(), GetShapeId()));
    }
}

task<void> StaticMeshComponent::WaitForMeshReady(StreamingAssetRef<StaticMesh> mesh, SceneId scene, ShapeId registration) {
    if (co_await mesh) {
        if (IsLive() && IsRegistered() && GetRenderSceneId() == scene && GetShapeId() == registration && _mesh == mesh && mesh.IsReady()) {
            MarkRenderStateDirty();
        }
    }
}

void StaticMeshComponent::StartMaterialReadyWaits() {
    if (!GetShapeId().IsValid()) return;
    for (const auto& material : _materials) {
        if (material.IsValid() && !material.IsCompleted()) {
            if (!_materialReadyWait) _materialReadyWait = make_unique<TaskScope>();
            _materialReadyWait->Spawn(WaitForMaterialReady(material, GetRenderSceneId(), GetShapeId()));
        }
    }
}

task<void> StaticMeshComponent::WaitForMaterialReady(StreamingAssetRef<Material> material, SceneId scene, ShapeId registration) {
    if (co_await material) {
        if (IsLive() && IsRegistered() && GetRenderSceneId() == scene && GetShapeId() == registration &&
            std::find(_materials.begin(), _materials.end(), material) != _materials.end() && material.IsReady()) MarkRenderDynamicDataDirty();
    }
}

void StaticMeshComponent::CollectPrimitiveUpdates(ShapeCapture& capture, RenderDirtyFlags dirty) {
    if (dirty.HasFlag(RenderDirtyFlag::State)) {
        capture.SetStaticMesh(_mesh, GetSceneTransformId());
    }
    if (dirty.HasFlag(RenderDirtyFlag::State) || dirty.HasFlag(RenderDirtyFlag::DynamicData)) capture.SetMaterials(_materials);
}

}  // namespace radray
