#pragma once

#include <radray/runtime/asset_manager.h>
#include <radray/runtime/material.h>
#include <radray/runtime/static_mesh.h>
#include <radray/runtime/components/primitive_component.h>

namespace radray {

/// 引用 StaticMesh 资产的静态网格组件。
/// 对应 UE5 的 UStaticMeshComponent。
///
/// 组件只持有 AssetRef<StaticMesh> + AssetRef<Material>(资产走 AssetManager),不碰 GPU 资源;
/// OnRegister 时通过 CreateSceneProxy 把资产引用 + 世界变换快照给渲染侧。GPU 上传由 Proxy
/// 在渲染系统帧顶 UpdateResources 时数据驱动完成,组件不参与。
class StaticMeshComponent : public PrimitiveComponent {
public:
    StaticMeshComponent() noexcept = default;
    explicit StaticMeshComponent(AssetRef<StaticMesh> mesh) noexcept : _mesh(std::move(mesh)) {}
    StaticMeshComponent(AssetRef<StaticMesh> mesh, AssetRef<Material> material) noexcept
        : _mesh(std::move(mesh)), _material(std::move(material)) {}
    ~StaticMeshComponent() noexcept override = default;

    void SetStaticMesh(AssetRef<StaticMesh> mesh) noexcept;
    const AssetRef<StaticMesh>& GetStaticMesh() const noexcept { return _mesh; }

    void SetMaterial(AssetRef<Material> material) noexcept { _material = std::move(material); }
    const AssetRef<Material>& GetMaterial() const noexcept { return _material; }

    unique_ptr<PrimitiveSceneProxy> CreateSceneProxy() override;

private:
    AssetRef<StaticMesh> _mesh;
    AssetRef<Material> _material;
};

}  // namespace radray
