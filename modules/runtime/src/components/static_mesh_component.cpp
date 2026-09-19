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
    if (GetPrimitiveId().IsValid() && _mesh.IsValid() && !_mesh.IsCompleted()) {
        _readyWait = make_unique<TaskScope>();
        _readyWait->Spawn(WaitForMeshReady(_mesh, GetRenderSceneId(), GetPrimitiveId()));
    }
}

task<void> StaticMeshComponent::WaitForMeshReady(StreamingAssetRef<StaticMesh> mesh, SceneId scene, PrimitiveId registration) {
    if (co_await mesh) {
        if (IsRegistered() && GetRenderSceneId() == scene && GetPrimitiveId() == registration && _mesh == mesh && mesh.IsReady()) {
            MarkRenderStateDirty();
        }
    }
}

void StaticMeshComponent::CollectPrimitiveUpdates(SceneWriter& writer, RenderDirtyFlags dirty) {
    if (dirty.HasFlag(RenderDirtyFlag::State)) {
        writer.SetStaticMesh(GetPrimitiveId(), _mesh, GetWorldMatrix());
    } else if (dirty.HasFlag(RenderDirtyFlag::Transform)) {
        writer.SetTransform(GetPrimitiveId(), GetWorldMatrix());
    }
}

}  // namespace radray
