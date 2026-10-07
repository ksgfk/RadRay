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
    void SetMaterials(vector<StreamingAssetRef<Material>> materials);
    std::span<const StreamingAssetRef<Material>> GetMaterials() const noexcept { return _materials; }
    const StreamingAssetRef<StaticMesh>& GetStaticMesh() const noexcept { return _mesh; }

private:
    bool UsesSceneTransform() const noexcept override { return true; }
    void OnRenderStateCreated() override;
    void OnRenderStateDestroyed() override;
    void StartMeshReadyWait();
    void StartMaterialReadyWaits();
    task<void> WaitForMaterialReady(StreamingAssetRef<Material> material, SceneId scene, ShapeId registration);
    task<void> WaitForMeshReady(StreamingAssetRef<StaticMesh> mesh, SceneId scene, ShapeId registration);
    void CollectPrimitiveUpdates(ShapeCapture& capture, RenderDirtyFlags dirty) override;

    StreamingAssetRef<StaticMesh> _mesh;
    vector<StreamingAssetRef<Material>> _materials;
    unique_ptr<TaskScope> _readyWait;
    unique_ptr<TaskScope> _materialReadyWait;
};

template <>
struct RuntimeTypeTrait<StaticMeshComponent> {
    static constexpr RuntimeTypeId value{0x7911a3cb, 0x45a7, 0x46d8, 0xa3, 0xb1, 0x71, 0xc4, 0xf7, 0x85, 0x32, 0x0e};
};

}  // namespace radray
