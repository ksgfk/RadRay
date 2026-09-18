#pragma once

#include <radray/runtime/asset_manager.h>
#include <radray/runtime/components/primitive_component.h>
#include <radray/runtime/static_mesh.h>

namespace radray {

class StaticMeshComponent final : public PrimitiveComponent {
public:
    StaticMeshComponent() noexcept = default;
    ~StaticMeshComponent() noexcept override;

    void SetStaticMesh(StreamingAssetRef<StaticMesh> mesh);
    const StreamingAssetRef<StaticMesh>& GetStaticMesh() const noexcept { return _mesh; }

private:
    void OnRegister() override;
    void OnUnregister() override;
    void StartMeshReadyWait();
    task<void> WaitForMeshReady(StreamingAssetRef<StaticMesh> mesh, PrimitiveId registration);
    void CollectPrimitiveUpdates(SceneUpdateBatch& batch, RenderDirtyFlags dirty) override;

    StreamingAssetRef<StaticMesh> _mesh;
    std::optional<AssetId> _sentMeshAssetId;
    unique_ptr<TaskScope> _readyWait;
};

template <>
struct RuntimeTypeTrait<StaticMeshComponent> {
    static constexpr RuntimeTypeId value{0x7911a3cb, 0x45a7, 0x46d8, 0xa3, 0xb1, 0x71, 0xc4, 0xf7, 0x85, 0x32, 0x0e};
};

}  // namespace radray
