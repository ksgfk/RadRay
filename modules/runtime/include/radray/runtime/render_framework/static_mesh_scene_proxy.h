#pragma once

#include <radray/nullable.h>
#include <radray/runtime/asset_manager.h>
#include <radray/runtime/render_framework/primitive_scene_proxy.h>
#include <radray/runtime/static_mesh.h>
#include <radray/types.h>

namespace radray {

class Material;

class StaticMeshSceneProxy final : public PrimitiveSceneProxy {
public:
    StaticMeshSceneProxy(
        StreamingAssetRef<StaticMesh> mesh,
        vector<Nullable<Material*>> materials,
        const Eigen::Matrix4f& localToWorld) noexcept;
    ~StaticMeshSceneProxy() noexcept override;

    void CollectAssetReferences(vector<StreamingAssetRefAny>& out) const override;
    uint64_t GetRenderDataRevision() const noexcept override;
    uint64_t GetTransformRevision() const noexcept override { return GetLocalToWorldRevision(); }
    bool UsesRenderChangeNotifications() const noexcept override { return true; }
    bool HasPendingRenderResources() const noexcept override;

    void SetStaticMesh(StreamingAssetRef<StaticMesh> mesh) noexcept;
    void SetMaterial(uint32_t sectionIndex, Nullable<Material*> material);

    AxisAlignedBounds GetLocalBounds() const noexcept override;
    MeshDrawArgs GetDrawArgs(uint32_t sectionIndex) const noexcept override;
    uint32_t GetSectionCount() const noexcept override;
    Nullable<Material*> GetMaterial(uint32_t sectionIndex) const noexcept override;

private:
    StreamingAssetRef<StaticMesh> _mesh;
    vector<Nullable<Material*>> _materials;
    uint64_t _renderDataRevision{1};
};

}  // namespace radray
