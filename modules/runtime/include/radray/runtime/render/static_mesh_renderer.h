#pragma once

#include <radray/types.h>
#include <radray/basic_math.h>
#include <radray/render/common.h>
#include <radray/runtime/render/renderer.h>

namespace radray {
class StaticMesh;
}

namespace radray::srp {

class Material;

/// 具体 Renderer:StaticMesh 的一个 section/primitive 的渲染镜像。替代旧 StaticMeshSceneProxy。
///
/// 一个 StaticMeshRenderer = 一次几何绘制单元(一个 MeshBatchElement)+ 一个 material 引用 + 世界矩阵。
/// 多 section 的网格 → 多个 StaticMeshRenderer(各引用同一 StaticMesh 的不同 drawData/range)。
///
/// 自包含顶点布局:从 StaticMesh 的 MeshResource 抽取语义,【拷贝语义字符串】到自身,
/// 避免 VertexElement.Semantic(string_view)悬垂于资产生命周期。非移动(span 指向自身存储)。
class StaticMeshRenderer final : public Renderer {
public:
    /// 构造一个覆盖整网格(所有 section/primitive 合并为一次 draw 不可行,故取指定 primitive/section)。
    /// 实践:gltf 每 primitive 一个 StaticMeshRenderer。primitiveIndex 索引 RenderMesh::_drawDatas。
    /// firstIndex/indexCount:section 子范围(整 primitive 传 0 / 该 primitive 的 IndexCount)。
    StaticMeshRenderer(
        StaticMesh* mesh,
        uint32_t primitiveIndex,
        uint32_t firstIndex,
        uint32_t indexCount,
        Material* material) noexcept;

    StaticMeshRenderer(const StaticMeshRenderer&) = delete;
    StaticMeshRenderer& operator=(const StaticMeshRenderer&) = delete;
    StaticMeshRenderer(StaticMeshRenderer&&) = delete;
    StaticMeshRenderer& operator=(StaticMeshRenderer&&) = delete;
    ~StaticMeshRenderer() noexcept override = default;

    MeshBatchElement BatchElement() const override { return _element; }
    const render::VertexBufferLayout& GetVertexLayout() const override { return _vertexLayout; }
    const Eigen::Matrix4f& WorldMatrix() const override { return _worldMatrix; }
    Material* GetMaterial() const override { return _material; }

    void SetWorldMatrix(const Eigen::Matrix4f& m) override { _worldMatrix = m; }
    void SetMaterial(Material* m) noexcept { _material = m; }

    /// 几何是否就绪(有有效 batch element 与顶点布局)。
    bool IsRenderable() const noexcept {
        return _element.Vbv.Target != nullptr && _element.Ibv.Target != nullptr &&
               _element.IndexCount != 0 && !_vertexElements.empty();
    }

private:
    void BuildGeometry(StaticMesh* mesh, uint32_t primitiveIndex, uint32_t firstIndex, uint32_t indexCount) noexcept;

    Material* _material{nullptr};
    Eigen::Matrix4f _worldMatrix{Eigen::Matrix4f::Identity()};

    MeshBatchElement _element{};
    // 顶点布局自包含:语义字符串拷贝进 _semanticStorage,VertexElement.Semantic 指向它。
    vector<string> _semanticStorage;
    vector<render::VertexElement> _vertexElements;
    render::VertexBufferLayout _vertexLayout{};
};

}  // namespace radray::srp
