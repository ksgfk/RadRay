#pragma once

#include <radray/runtime/asset_manager.h>
#include <radray/runtime/components/primitive_component.h>
#include <radray/runtime/material_asset.h>
#include <radray/runtime/static_mesh.h>

namespace radray {

class StaticMeshSceneProxy;

/// Component that renders a StaticMesh asset.
/// Corresponds to UE5's UStaticMeshComponent.
class StaticMeshComponent : public PrimitiveComponent {
public:
    StaticMeshComponent() noexcept = default;
    ~StaticMeshComponent() noexcept override;

    RuntimeTypeId GetTypeId() const noexcept override;
    void OnRegister() override;
    void OnUnregister() override;
    void TickComponent(float deltaTime) override;

    void SetStaticMesh(StreamingAssetRef<StaticMesh> mesh);
    const StreamingAssetRef<StaticMesh>& GetStaticMesh() const noexcept { return _mesh; }
    bool ShouldCreateRenderState() const override;
    unique_ptr<PrimitiveSceneProxy> CreateSceneProxy() override;

    /// 设置 section 材质槽 (对应 UE 的 UPrimitiveComponent::SetMaterial)。
    /// 材质槽持 StreamingAssetRef (generation 检测兜底悬垂): 材质被 Unload 后 Get() 自动返回空。
    /// game 线程 Tick 时据此生成渲染快照发布给 proxy。
    void SetMaterial(uint32_t sectionIndex, StreamingAssetRef<MaterialAsset> material) noexcept;
    StreamingAssetRef<MaterialAsset> GetMaterial(uint32_t sectionIndex) const noexcept;
    uint32_t GetMaterialSlotCount() const noexcept { return static_cast<uint32_t>(_sectionMaterials.size()); }

private:
    bool HasRenderableMesh() const noexcept;
    void CleanupCompletedMeshRefreshTask() noexcept;
    void StopMeshRefreshTask() noexcept;
    void StartMeshRefreshTask();
    task<void> WaitForMeshAndRefresh(StreamingAssetRef<StaticMesh> mesh);
    bool IsCurrentMesh(const StreamingAssetRef<StaticMesh>& mesh) const noexcept;
    /// 为所有 section 生成材质快照并发布给 proxy (game 线程调用)。
    void RefreshMaterialSnapshots(StaticMeshSceneProxy& proxy) const noexcept;

    StreamingAssetRef<StaticMesh> _mesh;
    unique_ptr<TaskScope> _meshRefreshScope;
    bool _meshRefreshCompleted{false};
    // per-section 材质槽 (下标即 section index)。Tick 时据此生成快照发布给 proxy。
    vector<StreamingAssetRef<MaterialAsset>> _sectionMaterials;
};

template <>
struct RuntimeTypeTrait<StaticMeshComponent> {
    static constexpr RuntimeTypeId value{0xa1c3650a, 0xf8bb, 0x42ed, 0xba, 0xa2, 0x7d, 0xa6, 0x28, 0xf2, 0xa2, 0x19};
};

}  // namespace radray
