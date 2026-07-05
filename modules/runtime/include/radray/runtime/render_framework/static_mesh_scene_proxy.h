#pragma once

#include <span>

#include <radray/basic_math.h>
#include <radray/render/gpu_resource.h>
#include <radray/runtime/asset_manager.h>
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
        // per-section 材质 (对应 UE5 FStaticMeshSection::MaterialIndex / Unity 的 submesh material)。
        // 非拥有: 生命周期由 material 持有方管理。为空则该 section 不参与绘制。
        MaterialAsset* Material{nullptr};
    };

    StaticMeshSceneProxy(const StaticMeshComponent& component, StreamingAssetRef<StaticMesh> mesh);
    ~StaticMeshSceneProxy() noexcept override;

    StaticMesh* GetStaticMesh() const noexcept { return _mesh.Get(); }
    const render::RenderMesh* GetRenderMesh() const noexcept;
    const StreamingAssetRef<StaticMesh>& GetStaticMeshRef() const noexcept { return _mesh; }
    Eigen::Matrix4f GetLocalToWorld() const noexcept override { return _localToWorld; }
    MeshDrawArgs GetDrawArgs(uint32_t sectionIndex) const noexcept override;
    uint32_t GetSectionCount() const noexcept override { return static_cast<uint32_t>(_sections.size()); }
    MaterialAsset* GetSectionMaterial(uint32_t sectionIndex) const noexcept override {
        return sectionIndex < _sections.size() ? _sections[sectionIndex].Material : nullptr;
    }
    const Eigen::Vector3f& GetLocalBoundsMin() const noexcept { return _localBoundsMin; }
    const Eigen::Vector3f& GetLocalBoundsMax() const noexcept { return _localBoundsMax; }
    std::span<const Section> GetSections() const noexcept { return _sections; }
    std::span<Section> GetSections() noexcept { return _sections; }

    /// 设置指定 section 的材质。越界忽略。
    void SetSectionMaterial(uint32_t sectionIndex, MaterialAsset* material) noexcept;

private:
    StreamingAssetRef<StaticMesh> _mesh;
    Eigen::Matrix4f _localToWorld;
    Eigen::Vector3f _localBoundsMin;
    Eigen::Vector3f _localBoundsMax;
    vector<Section> _sections;
};

}  // namespace radray
