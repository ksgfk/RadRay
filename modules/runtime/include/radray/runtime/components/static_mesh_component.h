#pragma once

#include <radray/runtime/asset_manager.h>
#include <radray/runtime/material.h>
#include <radray/runtime/static_mesh.h>
#include <radray/runtime/components/primitive_component.h>

namespace radray {

/// 引用 StaticMesh 资产的静态网格组件。
/// 对应 UE5 的 UStaticMeshComponent。
///
/// 资产异步加载:组件持有 StreamingAssetRef,每帧 TickComponent 检查资产是否就绪；
/// 两者就绪后创建并注册 SceneProxy(资产就绪后才建代理,代理一经创建即可绘制)。
/// 组件不碰 GPU 资源。
class StaticMeshComponent : public PrimitiveComponent {
public:
    StaticMeshComponent() noexcept = default;
    ~StaticMeshComponent() noexcept override = default;

    /// 设置 streaming 资产引用(mesh 必需,material 可选)。组件会在两者均就绪后建代理。
    void SetStaticMesh(StreamingAssetRef<StaticMesh> mesh) noexcept { _mesh = std::move(mesh); }
    void SetMaterial(StreamingAssetRef<Material> material) noexcept { _material = std::move(material); }

    const StreamingAssetRef<StaticMesh>& GetStaticMesh() const noexcept { return _mesh; }
    const StreamingAssetRef<Material>& GetMaterial() const noexcept { return _material; }

    void TickComponent(float deltaTime) override;
    unique_ptr<PrimitiveSceneProxy> CreateSceneProxy() override;

private:
    bool AreAssetsReady() const noexcept;

    StreamingAssetRef<StaticMesh> _mesh;
    StreamingAssetRef<Material> _material;
};

}  // namespace radray
