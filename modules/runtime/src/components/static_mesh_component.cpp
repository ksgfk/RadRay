#include <radray/runtime/components/static_mesh_component.h>

#include <utility>

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

void StaticMeshComponent::OnRenderStateCreated() {
    StartMeshReadyWait();
}

void StaticMeshComponent::OnRenderStateDestroyed() {
    _readyWait.reset();
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

void StaticMeshComponent::CollectPrimitiveUpdates(ShapeCapture& capture, RenderDirtyFlags dirty) {
    if (dirty.HasFlag(RenderDirtyFlag::State)) {
        capture.SetStaticMesh(_mesh, GetWorldTransform());
    } else if (dirty.HasFlag(RenderDirtyFlag::Transform)) {
        capture.SetTransform(GetWorldTransform());
    }
}

}  // namespace radray
