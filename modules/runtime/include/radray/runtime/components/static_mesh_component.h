#pragma once

#include <radray/runtime/asset_manager.h>
#include <radray/runtime/components/primitive_component.h>
#include <radray/runtime/material_asset.h>
#include <radray/runtime/render_framework/material_property_block.h>
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
    /// 组件是快照的【权威生产者】: 它决定下一帧每个 section 用哪个材质。
    /// 写槽仅置脏, 实际快照重建统一在 Tick 的 RefreshMaterialSnapshots 中按需完成。
    void SetMaterial(uint32_t sectionIndex, StreamingAssetRef<MaterialAsset> material) noexcept;
    StreamingAssetRef<MaterialAsset> GetMaterial(uint32_t sectionIndex) const noexcept;
    uint32_t GetMaterialSlotCount() const noexcept { return static_cast<uint32_t>(_sectionMaterials.size()); }

    /// 手动标记某 section 的材质为脏, 下一帧强制重建快照。
    /// 已知限制: MaterialAsset 的内部属性 (SetFloat 等) 变更没有 generation/版本号,
    /// 脏重建无法自动感知同一 asset 内部参数被改。调用方改了参数后须显式调用本方法。
    void MarkMaterialDirty(uint32_t sectionIndex) noexcept;

    /// 设置某 section 的 per-primitive 参数覆盖 (对应 Unity 的 Renderer.SetPropertyBlock)。
    /// 覆盖值在生成快照时叠加到共享材质模板之上, 不修改共享 MaterialAsset。
    /// 传 nullptr 清除覆盖。block 的内部参数变更 (SetFloat 等) 通过其版本号自动感知,
    /// 无需手动标脏 (区别于 MaterialAsset)。
    void SetPropertyBlock(uint32_t sectionIndex, shared_ptr<MaterialPropertyBlock> block) noexcept;
    /// 取某 section 的参数覆盖 (未设置返回 nullptr)。
    Nullable<MaterialPropertyBlock*> GetPropertyBlock(uint32_t sectionIndex) const noexcept;
    /// 清除某 section 的参数覆盖 (回落到纯共享材质)。
    void ClearPropertyBlock(uint32_t sectionIndex) noexcept;

private:
    /// per-section 材质槽 (下标即 section index)。组件据此生成快照发布给 proxy。
    /// 记录上一次快照据以生成的 handle/指针, 用于脏重建判定 (避免每帧全量克隆)。
    struct MaterialSlot {
        StreamingAssetRef<MaterialAsset> Material{};
        shared_ptr<MaterialPropertyBlock> PropertyBlock{};  // per-primitive 参数覆盖 (可空)
        AssetHandle LastHandle{AssetHandle::Invalid()};  // 上次快照的 handle (generation 兜底)
        MaterialAsset* LastPtr{nullptr};                 // 上次快照的解析指针 (Loading->Ready 跳变检测)
        uint64_t LastBlockVersion{0};                    // 上次快照据以生成的 block 版本号
        const MaterialPropertyBlock* LastBlockPtr{nullptr};  // 上次快照的 block 指针 (换 block 检测)
        bool Dirty{true};                                // 是否需要重建快照
    };

    bool HasRenderableMesh() const noexcept;
    void CleanupCompletedMeshRefreshTask() noexcept;
    void StopMeshRefreshTask() noexcept;
    void StartMeshRefreshTask();
    task<void> WaitForMeshAndRefresh(StreamingAssetRef<StaticMesh> mesh);
    bool IsCurrentMesh(const StreamingAssetRef<StaticMesh>& mesh) const noexcept;
    /// 为脏 section 重建材质快照并发布给 proxy (game 线程调用)。无变更的 section 空转。
    void RefreshMaterialSnapshots(StaticMeshSceneProxy& proxy) noexcept;

    StreamingAssetRef<StaticMesh> _mesh;
    unique_ptr<TaskScope> _meshRefreshScope;
    bool _meshRefreshCompleted{false};
    vector<MaterialSlot> _sectionMaterials;
};

template <>
struct RuntimeTypeTrait<StaticMeshComponent> {
    static constexpr RuntimeTypeId value{0xa1c3650a, 0xf8bb, 0x42ed, 0xba, 0xa2, 0x7d, 0xa6, 0x28, 0xf2, 0xa2, 0x19};
    using Bases = std::tuple<PrimitiveComponent>;
};

}  // namespace radray
