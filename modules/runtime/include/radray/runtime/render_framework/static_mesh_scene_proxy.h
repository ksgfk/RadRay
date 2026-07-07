#pragma once

#include <atomic>
#include <span>

#include <radray/basic_math.h>
#include <radray/render/gpu_resource.h>
#include <radray/runtime/asset_manager.h>
#include <radray/runtime/material_asset.h>
#include <radray/runtime/render_framework/primitive_scene_proxy.h>
#include <radray/runtime/static_mesh.h>
#include <radray/types.h>

namespace radray {

class StaticMesh;
class StaticMeshComponent;
class MaterialAsset;

/// Render-side proxy for StaticMeshComponent.
/// Corresponds to UE5's FStaticMeshSceneProxy.
class StaticMeshSceneProxy final : public PrimitiveSceneProxy {
public:
    struct Section {
        uint32_t PrimitiveIndex{0};  // 索引 RenderMesh._drawDatas (VB/IB view)
        uint32_t FirstIndex{0};
        uint32_t IndexCount{0};
        uint32_t MinVertexIndex{0};
        uint32_t MaxVertexIndex{0};
    };

    StaticMeshSceneProxy(const StaticMeshComponent& component, StreamingAssetRef<StaticMesh> mesh);
    ~StaticMeshSceneProxy() noexcept override;

    StaticMesh* GetStaticMesh() const noexcept { return _mesh.Get(); }
    const render::RenderMesh* GetRenderMesh() const noexcept;
    const StreamingAssetRef<StaticMesh>& GetStaticMeshRef() const noexcept { return _mesh; }
    Eigen::Matrix4f GetLocalToWorld() const noexcept override { return _localToWorld; }
    MeshDrawArgs GetDrawArgs(uint32_t sectionIndex) const noexcept override;
    uint32_t GetSectionCount() const noexcept override { return static_cast<uint32_t>(_sections.size()); }
    shared_ptr<const MaterialRenderSnapshot> GetSectionSnapshot(uint32_t sectionIndex) const noexcept override {
        if (sectionIndex >= _sectionSnapshots.size()) {
            return nullptr;
        }
        return _sectionSnapshots[sectionIndex]->load(std::memory_order_acquire);
    }
    const Eigen::Vector3f& GetLocalBoundsMin() const noexcept { return _localBoundsMin; }
    const Eigen::Vector3f& GetLocalBoundsMax() const noexcept { return _localBoundsMax; }
    std::span<const Section> GetSections() const noexcept { return _sections; }
    std::span<Section> GetSections() noexcept { return _sections; }

    /// 发布指定 section 的材质快照 (game 线程写, render 线程无锁读)。越界忽略。
    void SetSectionSnapshot(uint32_t sectionIndex, shared_ptr<const MaterialRenderSnapshot> snapshot) noexcept;

private:
    using AtomicSnapshot = std::atomic<shared_ptr<const MaterialRenderSnapshot>>;

    StreamingAssetRef<StaticMesh> _mesh;
    Eigen::Matrix4f _localToWorld;
    Eigen::Vector3f _localBoundsMin;
    Eigen::Vector3f _localBoundsMax;
    vector<Section> _sections;
    // 与 _sections 平行的快照槽。atomic<shared_ptr> 不可移动, 故堆分配保证地址稳定,
    // 用 unique_ptr 使外层 vector 可移动/可增长。
    vector<unique_ptr<AtomicSnapshot>> _sectionSnapshots;
};

}  // namespace radray
