#include <radray/runtime/components/static_mesh_component.h>

#include <utility>

#include <radray/scope_guard.h>
#include <radray/runtime/application.h>
#include <radray/runtime/game_framework/world.h>
#include <radray/runtime/render_framework/static_mesh_scene_proxy.h>

namespace radray {

StaticMeshComponent::~StaticMeshComponent() noexcept {
    StopMeshRefreshTask();
}

RuntimeTypeId StaticMeshComponent::GetTypeId() const noexcept {
    return runtime_type_id_v<StaticMeshComponent>;
}

void StaticMeshComponent::OnRegister() {
    PrimitiveComponent::OnRegister();
    StartMeshRefreshTask();
}

void StaticMeshComponent::OnUnregister() {
    StopMeshRefreshTask();
    PrimitiveComponent::OnUnregister();
}

void StaticMeshComponent::TickComponent(float deltaTime) {
    PrimitiveComponent::TickComponent(deltaTime);
    CleanupCompletedMeshRefreshTask();
}

void StaticMeshComponent::SetStaticMesh(StreamingAssetRef<StaticMesh> mesh) {
    StopMeshRefreshTask();
    _mesh = std::move(mesh);
    MarkRenderStateDirty();
    StartMeshRefreshTask();
}

bool StaticMeshComponent::ShouldCreateRenderState() const {
    return PrimitiveComponent::ShouldCreateRenderState() && HasRenderableMesh();
}

unique_ptr<PrimitiveSceneProxy> StaticMeshComponent::CreateSceneProxy() {
    if (!HasRenderableMesh()) {
        return nullptr;
    }

    return make_unique<StaticMeshSceneProxy>(*this, _mesh);
}

bool StaticMeshComponent::HasRenderableMesh() const noexcept {
    StaticMesh* mesh = _mesh.Get();
    return mesh != nullptr && mesh->HasRenderData();
}

void StaticMeshComponent::CleanupCompletedMeshRefreshTask() noexcept {
    if (!_meshRefreshCompleted || _meshRefreshScope == nullptr) {
        return;
    }

    _meshRefreshScope->WaitUntilEmpty();
    _meshRefreshScope.reset();
    _meshRefreshCompleted = false;
}

void StaticMeshComponent::StopMeshRefreshTask() noexcept {
    if (_meshRefreshScope == nullptr) {
        _meshRefreshCompleted = false;
        return;
    }

    _meshRefreshScope->RequestStop();
    _meshRefreshScope->WaitUntilEmpty();
    _meshRefreshScope.reset();
    _meshRefreshCompleted = false;
}

void StaticMeshComponent::StartMeshRefreshTask() {
    if (!IsRegistered() || !_mesh.IsValid() || _mesh.IsCompleted() || _meshRefreshScope != nullptr) {
        return;
    }

    _meshRefreshCompleted = false;
    _meshRefreshScope = make_unique<TaskScope>();
    _meshRefreshScope->Spawn(WaitForMeshAndRefresh(_mesh));
}

task<void> StaticMeshComponent::WaitForMeshAndRefresh(StreamingAssetRef<StaticMesh> mesh) {
    auto completion = MakeScopeGuard([this]() noexcept {
        _meshRefreshCompleted = true;
    });

    Nullable<World*> world = GetWorld();
    if (!world) {
        co_return;
    }
    Application* app = world.Get()->GetApplication();
    if (app == nullptr) {
        co_return;
    }

    AssetManager* assetManager = app->GetAssetManager();
    if (assetManager == nullptr) {
        co_return;
    }

    co_await assetManager->Wait(mesh);

    co_await app->GetScheduler().SwitchTo();

    if (!IsRegistered() || !IsCurrentMesh(mesh) || !mesh.IsReady()) {
        co_return;
    }
    MarkRenderStateDirty();
}

bool StaticMeshComponent::IsCurrentMesh(const StreamingAssetRef<StaticMesh>& mesh) const noexcept {
    return _mesh.GetHandle() == mesh.GetHandle() && _mesh.GetAssetId() == mesh.GetAssetId();
}

}  // namespace radray
