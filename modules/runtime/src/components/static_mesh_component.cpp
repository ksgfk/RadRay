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
    // game 线程每帧从材质槽生成只读快照发布给 proxy (render 线程无锁读)。
    if (auto* proxy = static_cast<StaticMeshSceneProxy*>(GetSceneProxy()); proxy != nullptr) {
        RefreshMaterialSnapshots(*proxy);
    }
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

    auto proxy = make_unique<StaticMeshSceneProxy>(*this, _mesh);
    // proxy 每次 (重新) 创建都以组件材质槽为权威来源生成快照发布,
    // 保证异步 mesh 加载 / MarkRenderStateDirty 重建后材质不丢。
    RefreshMaterialSnapshots(*proxy);
    return proxy;
}

void StaticMeshComponent::SetMaterial(uint32_t sectionIndex, StreamingAssetRef<MaterialAsset> material) noexcept {
    if (sectionIndex >= _sectionMaterials.size()) {
        _sectionMaterials.resize(sectionIndex + 1);
    }
    _sectionMaterials[sectionIndex] = std::move(material);
    // proxy 已存在则立即发布快照; 否则等 CreateSceneProxy / 下一次 Tick 统一发布。
    if (auto* proxy = static_cast<StaticMeshSceneProxy*>(GetSceneProxy()); proxy != nullptr) {
        if (sectionIndex < proxy->GetSectionCount()) {
            MaterialAsset* mat = _sectionMaterials[sectionIndex].Get();
            proxy->SetSectionSnapshot(sectionIndex, mat != nullptr ? mat->CreateSnapshot() : nullptr);
        }
    }
}

StreamingAssetRef<MaterialAsset> StaticMeshComponent::GetMaterial(uint32_t sectionIndex) const noexcept {
    return sectionIndex < _sectionMaterials.size() ? _sectionMaterials[sectionIndex] : StreamingAssetRef<MaterialAsset>{};
}

void StaticMeshComponent::RefreshMaterialSnapshots(StaticMeshSceneProxy& proxy) const noexcept {
    const uint32_t sectionCount = proxy.GetSectionCount();
    const uint32_t slotCount = static_cast<uint32_t>(_sectionMaterials.size());
    const uint32_t count = sectionCount < slotCount ? sectionCount : slotCount;
    for (uint32_t s = 0; s < count; ++s) {
        MaterialAsset* mat = _sectionMaterials[s].Get();
        proxy.SetSectionSnapshot(s, mat != nullptr ? mat->CreateSnapshot() : nullptr);
    }
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
