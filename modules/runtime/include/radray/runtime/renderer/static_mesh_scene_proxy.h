#pragma once

#include <radray/types.h>
#include <radray/render/common.h>
#include <radray/runtime/asset_manager.h>
#include <radray/runtime/material.h>
#include <radray/runtime/material_instance.h>
#include <radray/runtime/material_render_proxy.h>
#include <radray/runtime/static_mesh.h>
#include <radray/runtime/renderer/primitive_scene_proxy.h>

namespace radray {

class GpuSystem;

/// StaticMesh 的渲染侧代理。对应 UE5 的 FStaticMeshSceneProxy。
///
/// 持有 StreamingAssetRef<StaticMesh> 与 StreamingAssetRef<Material>(非拥有弱句柄),把每个 section/primitive
/// 翻译成 MeshBatchElement,并向渲染器暴露材质与顶点布局。
/// 资产在构造代理前已由 AssetManager 保证 GPU 就绪(StaticMesh 构造即完整),
/// 故代理在构造函数中直接构建几何单元,无需帧顶轮询上传。
class StaticMeshSceneProxy : public PrimitiveSceneProxy {
public:
    StaticMeshSceneProxy(StreamingAssetRef<StaticMesh> mesh, StreamingAssetRef<Material> material) noexcept;
    StaticMeshSceneProxy(
        StreamingAssetRef<StaticMesh> mesh,
        StreamingAssetRef<Material> material,
        vector<MaterialParameterAssignment> materialParams) noexcept;
    StaticMeshSceneProxy(
        StreamingAssetRef<StaticMesh> mesh,
        StreamingAssetRef<Material> material,
        vector<MaterialParameterAssignment> materialParams,
        vector<MaterialTextureAssignment> materialTextures) noexcept;
    StaticMeshSceneProxy(
        StreamingAssetRef<StaticMesh> mesh,
        StreamingAssetRef<Material> material,
        vector<MaterialParameterAssignment> materialParams,
        vector<MaterialTextureAssignment> materialTextures,
        bool isTransparent) noexcept;
    ~StaticMeshSceneProxy() noexcept override;

    bool IsRenderable() const noexcept override;
    bool IsTransparent() const noexcept override { return _isTransparent; }
    void CollectBatchElements(vector<MeshBatchElement>& out) const override;
    Material* GetMaterial() const noexcept override { return _material.Get(); }
    const render::VertexBufferLayout& GetVertexLayout() const noexcept override {
        return _vertexLayout;
    }

    render::DescriptorSet* GetMaterialDescriptorSet(GpuSystem* gpuSystem) const override;
    render::DescriptorSetIndex GetMaterialSetIndex() const noexcept override;

    StaticMesh* GetStaticMesh() const noexcept { return _mesh.Get(); }

private:
    /// 从已就绪的 RenderMesh + Sections 构建几何绘制单元与顶点布局。由构造函数调用一次。
    void BuildGeometry() noexcept;
    /// 从 Material 创建 MaterialInstance 并写入 per-使用点材质参数。由构造函数调用一次。
    void BuildMaterialInstance() noexcept;

    StreamingAssetRef<StaticMesh> _mesh;
    StreamingAssetRef<Material> _material;
    vector<MeshBatchElement> _batchElements;
    // 顶点布局的元素存储与其 span 视图;span 引用 _vertexElements,二者生命周期一致。
    vector<render::VertexElement> _vertexElements;
    render::VertexBufferLayout _vertexLayout;

    // per-material 参数实例 + GPU 代理。静态材质:首次索取时懒构建 proxy 并缓存。
    vector<MaterialParameterAssignment> _materialParams;
    vector<MaterialTextureAssignment> _materialTextures;
    bool _isTransparent{false};
    MaterialInstance _materialInstance;
    mutable MaterialRenderProxy _materialRenderProxy;
    mutable bool _materialProxyBuilt{false};
    mutable bool _materialProxyFailed{false};
};

}  // namespace radray
