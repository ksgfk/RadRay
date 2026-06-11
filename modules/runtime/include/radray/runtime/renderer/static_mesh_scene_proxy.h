#pragma once

#include <radray/types.h>
#include <radray/render/common.h>
#include <radray/runtime/asset_manager.h>
#include <radray/runtime/material.h>
#include <radray/runtime/static_mesh.h>
#include <radray/runtime/renderer/primitive_scene_proxy.h>

namespace radray {

/// StaticMesh 的渲染侧代理。对应 UE5 的 FStaticMeshSceneProxy。
///
/// 持有 AssetRef<StaticMesh> 与 AssetRef<Material> 保活资产,把每个 section/primitive
/// 翻译成 MeshBatchElement,并向渲染器暴露材质与顶点布局。
/// 组件层只传资产引用 + 世界变换,本类不创建任何 GPU 资源。
/// GPU 资源由渲染系统数据驱动:每帧帧顶对 Pending 态代理调用 UpdateResources,
/// 由本类经 ResourceUploader 上传网格,完成后转入 Ready 并构建几何单元。
class StaticMeshSceneProxy : public PrimitiveSceneProxy {
public:
    StaticMeshSceneProxy(AssetRef<StaticMesh> mesh, AssetRef<Material> material) noexcept;
    ~StaticMeshSceneProxy() noexcept override;

    bool IsRenderable() const noexcept override;
    void UpdateResources(render::CommandBuffer* cmdBuffer, ResourceUploader& uploader) override;
    void CollectBatchElements(vector<MeshBatchElement>& out) const override;
    Material* GetMaterial() const noexcept override { return _material.Get(); }
    const render::VertexBufferLayout& GetVertexLayout() const noexcept override {
        return _vertexLayout;
    }

    StaticMesh* GetStaticMesh() const noexcept { return _mesh.Get(); }

private:
    /// 从已就绪的 RenderMesh + Sections 构建几何绘制单元与顶点布局。
    /// 由 UpdateResources 在 GPU 数据就绪后调用一次。
    void BuildGeometry() noexcept;

    AssetRef<StaticMesh> _mesh;
    AssetRef<Material> _material;
    vector<MeshBatchElement> _batchElements;
    // 顶点布局的元素存储与其 span 视图;span 引用 _vertexElements,二者生命周期一致。
    vector<render::VertexElement> _vertexElements;
    render::VertexBufferLayout _vertexLayout;
};

}  // namespace radray
