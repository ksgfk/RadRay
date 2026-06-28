#pragma once

#include <radray/runtime/asset_manager.h>
#include <radray/runtime/components/primitive_component.h>
#include <radray/runtime/static_mesh.h>

namespace radray {

/// Component that renders a StaticMesh asset.
/// Corresponds to UE5's UStaticMeshComponent.
class StaticMeshComponent : public PrimitiveComponent {
public:
    StaticMeshComponent() noexcept = default;
    ~StaticMeshComponent() noexcept override;

    RuntimeTypeId GetTypeId() const noexcept override;
    void OnRegister() override;
    void OnUnregister() override;

    void SetStaticMesh(StreamingAssetRef<StaticMesh> mesh);
    const StreamingAssetRef<StaticMesh>& GetStaticMesh() const noexcept { return _mesh; }
    unique_ptr<PrimitiveSceneProxy> CreateSceneProxy() override;

private:
    void StopMeshRefreshTask() noexcept;
    void StartMeshRefreshTask();
    task<void> WaitForMeshAndRefresh(StreamingAssetRef<StaticMesh> mesh);
    bool IsCurrentMesh(const StreamingAssetRef<StaticMesh>& mesh) const noexcept;

    StreamingAssetRef<StaticMesh> _mesh;
    unique_ptr<TaskScope> _meshRefreshScope;
};

template <>
struct RuntimeTypeTrait<StaticMeshComponent> {
    static constexpr RuntimeTypeId value{0xa1c3650a, 0xf8bb, 0x42ed, 0xba, 0xa2, 0x7d, 0xa6, 0x28, 0xf2, 0xa2, 0x19};
};

}  // namespace radray
