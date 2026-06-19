#pragma once

#include <radray/runtime/asset_manager.h>
#include <radray/runtime/material.h>
#include <radray/runtime/material_instance.h>
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
    /// 设置 per-使用点材质参数(按名写入材质实例的 gMaterial cbuffer)。
    void SetMaterialParams(vector<MaterialParameterAssignment> params) noexcept { _materialParams = std::move(params); }
    /// 设置 per-使用点材质贴图(按名绑定材质实例的 texture 槽)。
    void SetMaterialTextures(vector<MaterialTextureAssignment> textures) noexcept { _materialTextures = std::move(textures); }

    const StreamingAssetRef<StaticMesh>& GetStaticMesh() const noexcept { return _mesh; }
    const StreamingAssetRef<Material>& GetMaterial() const noexcept { return _material; }
    const vector<MaterialParameterAssignment>& GetMaterialParams() const noexcept { return _materialParams; }
    const vector<MaterialTextureAssignment>& GetMaterialTextures() const noexcept { return _materialTextures; }

    void TickComponent(float deltaTime) override;
    unique_ptr<PrimitiveSceneProxy> CreateSceneProxy() override;

private:
    bool AreAssetsReady() const noexcept;

    StreamingAssetRef<StaticMesh> _mesh;
    StreamingAssetRef<Material> _material;
    vector<MaterialParameterAssignment> _materialParams;
    vector<MaterialTextureAssignment> _materialTextures;
};

}  // namespace radray
