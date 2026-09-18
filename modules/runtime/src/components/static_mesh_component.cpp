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

void StaticMeshComponent::OnRegister() {
    StartMeshReadyWait();
}

void StaticMeshComponent::OnUnregister() {
    _readyWait.reset();
    GetWorld()->UpdateRenderAssetUse(_sentMeshAssetId, nullptr);
    _sentMeshAssetId.reset();
}

void StaticMeshComponent::StartMeshReadyWait() {
    if (IsRegistered() && _mesh.IsValid() && !_mesh.IsCompleted()) {
        _readyWait = make_unique<TaskScope>();
        _readyWait->Spawn(WaitForMeshReady(_mesh, GetPrimitiveId()));
    }
}

task<void> StaticMeshComponent::WaitForMeshReady(StreamingAssetRef<StaticMesh> mesh, PrimitiveId registration) {
    if (co_await mesh) {
        if (IsRegistered() && GetPrimitiveId() == registration && _mesh == mesh && mesh.IsReady()) {
            MarkRenderStateDirty();
        }
    }
}

void StaticMeshComponent::CollectPrimitiveUpdates(SceneUpdateBatch& batch, RenderDirtyFlags dirty) {
    if (dirty.HasFlag(RenderDirtyFlag::State)) {
        StaticMeshStateUpdate update;
        update.Id = GetPrimitiveId();
        update.LocalToWorld = GetWorldMatrix();
        update.Mesh.MeshAssetId = _mesh.GetAssetId();
        Nullable<const StaticMesh*> renderAsset{nullptr};
        if (auto mesh = _mesh.Get(); mesh && mesh->IsValid()) {
            renderAsset = mesh.Get();
            update.Mesh.RenderMesh = &mesh->GetRenderMesh();
            update.Mesh.LocalBoundsMin = mesh->GetBoundsMin();
            update.Mesh.LocalBoundsMax = mesh->GetBoundsMax();
            update.Mesh.Sections = mesh->GetSections();
            if (update.Mesh.Sections.empty()) {
                const auto& primitives = mesh->GetMeshResource().Primitives;
                update.Mesh.Sections.reserve(primitives.size());
                for (size_t i = 0; i < primitives.size(); ++i) {
                    const auto& primitive = primitives[i];
                    update.Mesh.Sections.emplace_back(static_cast<uint32_t>(i), 0, primitive.IndexBuffer.IndexCount, 0, primitive.VertexCount - 1);
                }
            }
        }
        batch.MeshStates.push_back(std::move(update));
        GetWorld()->UpdateRenderAssetUse(_sentMeshAssetId, renderAsset);
        _sentMeshAssetId = renderAsset ? std::optional<AssetId>{renderAsset->GetAssetId()} : std::nullopt;
    } else if (dirty.HasFlag(RenderDirtyFlag::Transform)) {
        batch.Transforms.push_back({GetPrimitiveId(), GetWorldMatrix()});
    }
}

}  // namespace radray
