#pragma once

#include <cstdint>

#include <radray/basic_math.h>
#include <radray/render/common.h>

namespace radray::srp {

class Material;

/// 一次几何绘制单元(仅几何)。等价 UE5 FMeshBatchElement / 现 MeshBatchElement。
/// 只承载顶点/索引视图与绘制范围,不含材质/PSO。
struct MeshBatchElement {
    render::VertexBufferView Vbv;
    render::IndexBufferView Ibv;
    uint32_t IndexCount{0};
    uint32_t FirstIndex{0};
    int32_t VertexOffset{0};
};

/// 被 cull 的对象:几何 + 语义 + material 引用。等价 Unity Renderer / UE5 FPrimitiveSceneProxy。
/// 【只引用 Material,不拥有其内容】。依赖方向严格单向:Renderer → Material → Shader。
///
/// 抽象接口:runtime 定义"可渲染对象要能回答什么";具体几何来源(StaticMesh 等)由 game 层实现。
/// 设计依据:srp_runtime_architecture.md §6、srp_runtime_design.md §5。
class Renderer {
public:
    virtual ~Renderer() = default;

    /// 几何绘制单元(VBV/IBV/range)。
    virtual MeshBatchElement BatchElement() const = 0;

    /// 顶点布局(供 PSO input layout)。
    virtual const render::VertexBufferLayout& GetVertexLayout() const = 0;

    /// 世界变换(per-object 数据来源之一)。
    virtual const Eigen::Matrix4f& WorldMatrix() const = 0;

    /// 设置世界变换(默认空实现;可变 renderer 覆写,组件 OnTransformChanged 调用)。
    virtual void SetWorldMatrix(const Eigen::Matrix4f&) {}

    /// —— 几何/实例语义(第一层意图谓词读这些)——
    virtual uint32_t LayerMask() const { return 0xFFFFFFFFu; }
    virtual bool CastsShadow() const { return true; }
    virtual bool IsVisible() const { return true; }

    /// 引用的材质(决定 shader/PSO/渲染状态)。不拥有。
    virtual Material* GetMaterial() const = 0;
};

}  // namespace radray::srp
