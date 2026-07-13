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
    // 新 proxy 的快照槽为空, 且 slot 可能因上一个 proxy 已非脏。强制全部置脏,
    // 以组件材质槽为权威来源全量重发, 保证异步 mesh 加载 / MarkRenderStateDirty 重建后材质不丢。
    for (MaterialSlot& slot : _sectionMaterials) {
        slot.Dirty = true;
    }
    RefreshMaterialSnapshots(*proxy);
    return proxy;
}

void StaticMeshComponent::SetMaterial(uint32_t sectionIndex, StreamingAssetRef<MaterialAsset> material) noexcept {
    if (sectionIndex >= _sectionMaterials.size()) {
        _sectionMaterials.resize(sectionIndex + 1);
    }
    // 只写槽 + 置脏, 统一在下一次 RefreshMaterialSnapshots 重建并发布。
    _sectionMaterials[sectionIndex].Material = std::move(material);
    _sectionMaterials[sectionIndex].Dirty = true;
}

StreamingAssetRef<MaterialAsset> StaticMeshComponent::GetMaterial(uint32_t sectionIndex) const noexcept {
    return sectionIndex < _sectionMaterials.size() ? _sectionMaterials[sectionIndex].Material : StreamingAssetRef<MaterialAsset>{};
}

void StaticMeshComponent::MarkMaterialDirty(uint32_t sectionIndex) noexcept {
    if (sectionIndex < _sectionMaterials.size()) {
        _sectionMaterials[sectionIndex].Dirty = true;
    }
}

void StaticMeshComponent::SetPropertyBlock(uint32_t sectionIndex, shared_ptr<MaterialPropertyBlock> block) noexcept {
    if (sectionIndex >= _sectionMaterials.size()) {
        _sectionMaterials.resize(sectionIndex + 1);
    }
    _sectionMaterials[sectionIndex].PropertyBlock = std::move(block);
    _sectionMaterials[sectionIndex].Dirty = true;
}

Nullable<MaterialPropertyBlock*> StaticMeshComponent::GetPropertyBlock(uint32_t sectionIndex) const noexcept {
    if (sectionIndex >= _sectionMaterials.size()) {
        return nullptr;
    }
    return _sectionMaterials[sectionIndex].PropertyBlock.get();
}

void StaticMeshComponent::ClearPropertyBlock(uint32_t sectionIndex) noexcept {
    if (sectionIndex >= _sectionMaterials.size()) {
        return;
    }
    if (_sectionMaterials[sectionIndex].PropertyBlock != nullptr) {
        _sectionMaterials[sectionIndex].PropertyBlock.reset();
        _sectionMaterials[sectionIndex].Dirty = true;
    }
}

void StaticMeshComponent::RefreshMaterialSnapshots(StaticMeshSceneProxy& proxy) noexcept {
    const uint32_t sectionCount = proxy.GetSectionCount();
    const uint32_t slotCount = static_cast<uint32_t>(_sectionMaterials.size());
    const uint32_t count = sectionCount < slotCount ? sectionCount : slotCount;
    for (uint32_t s = 0; s < count; ++s) {
        MaterialSlot& slot = _sectionMaterials[s];
        MaterialAsset* mat = slot.Material.Get();
        const AssetHandle handle = slot.Material.GetHandle();
        const uint64_t materialRevision = mat != nullptr ? mat->GetRevision() : 0;
        const MaterialPropertyBlock* block = slot.PropertyBlock.get();
        const uint64_t blockVersion = block != nullptr ? block->GetVersion() : 0;
        // 脏判定: 显式置脏 / 解析指针变化 (Loading->Ready) / handle 变化 (换材质或 slot 回收复用)
        //         / 覆盖 block 换了 / 覆盖 block 内部参数版本变了。
        const bool needRebuild = slot.Dirty ||
                                 mat != slot.LastPtr ||
                                 handle != slot.LastHandle ||
                                 materialRevision != slot.LastMaterialRevision ||
                                 block != slot.LastBlockPtr ||
                                 blockVersion != slot.LastBlockVersion;
        if (!needRebuild) {
            continue;
        }
        proxy.SetSectionSnapshot(s, mat != nullptr ? mat->CreateSnapshot(block) : nullptr);
        slot.LastPtr = mat;
        slot.LastHandle = handle;
        slot.LastMaterialRevision = materialRevision;
        slot.LastBlockPtr = block;
        slot.LastBlockVersion = blockVersion;
        slot.Dirty = false;
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
