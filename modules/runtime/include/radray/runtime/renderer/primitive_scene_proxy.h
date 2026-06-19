#pragma once

#include <radray/basic_math.h>
#include <radray/types.h>
#include <radray/render/common.h>

namespace radray::render {
class CommandBuffer;
}

namespace radray {

class Material;
class GpuSystem;
class ResourceUploader;

/// 渲染侧的一次几何绘制单元(仅几何)。RadRay 版的 UE5 FMeshBatchElement。
/// 只承载顶点/索引视图与绘制范围,不含材质/PSO。由 SceneProxy 产出,
/// 经 MeshPassProcessor 与材质/PSO 组合成可执行的 MeshDrawCommand。
struct MeshBatchElement {
    render::VertexBufferView Vbv;
    render::IndexBufferView Ibv;
    uint32_t IndexCount{0};
    uint32_t FirstIndex{0};
    int32_t VertexOffset{0};
};

/// Render-side mirror of a PrimitiveComponent.
/// Holds GPU resources and produces MeshBatchElements + 关联材质/顶点布局。
/// 对应 UE5 的 FPrimitiveSceneProxy。
class PrimitiveSceneProxy {
public:
    PrimitiveSceneProxy() noexcept = default;
    PrimitiveSceneProxy(const PrimitiveSceneProxy&) = delete;
    PrimitiveSceneProxy(PrimitiveSceneProxy&&) = delete;
    PrimitiveSceneProxy& operator=(const PrimitiveSceneProxy&) = delete;
    PrimitiveSceneProxy& operator=(PrimitiveSceneProxy&&) = delete;
    virtual ~PrimitiveSceneProxy() noexcept = default;

    /// 获取世界变换矩阵
    const Eigen::Matrix4f& GetWorldMatrix() const noexcept { return _worldMatrix; }
    void SetWorldMatrix(const Eigen::Matrix4f& matrix) noexcept { _worldMatrix = matrix; }

    /// 是否可被渲染(GPU 数据就绪)。默认 false,派生类按需覆写。
    /// 资产在构造代理前已由 AssetManager 保证 GPU 就绪,故代理构造即可渲染。
    virtual bool IsRenderable() const noexcept { return false; }

    /// 收集本代理的几何绘制单元。MeshPassProcessor 据此 + 材质 + GetWorldMatrix()
    /// 解析出完整 MeshDrawCommand。对应 UE5 FPrimitiveSceneProxy::DrawStaticElements。
    /// 追加而非清空 out,便于一次性收集全场景。
    virtual void CollectBatchElements(vector<MeshBatchElement>& out) const { (void)out; }

    /// 本代理关联的材质(决定 shader/PSO/渲染状态)。默认无材质。
    virtual Material* GetMaterial() const noexcept { return nullptr; }

    /// 本代理的 per-material descriptor set(已写入常量缓冲/贴图)。
    /// 需要 GPU 设备首次懒构建;静态材质一次构建后缓存。
    /// 返回 nullptr 表示无 per-material 绑定(录制时跳过)。
    /// 对应 UE5 FPrimitiveSceneProxy 提供 FMaterialRenderProxy 的职责。
    virtual render::DescriptorSet* GetMaterialDescriptorSet(GpuSystem* gpuSystem) const {
        (void)gpuSystem;
        return nullptr;
    }

    /// per-material set 索引(register space)。仅在 GetMaterialDescriptorSet 返回非空时有意义。
    virtual render::DescriptorSetIndex GetMaterialSetIndex() const noexcept { return render::DescriptorSetIndex{1}; }

    /// 本代理几何对应的顶点布局(供 PSO input layout)。默认空布局。
    virtual const render::VertexBufferLayout& GetVertexLayout() const noexcept {
        static const render::VertexBufferLayout kEmpty{};
        return kEmpty;
    }

protected:
    Eigen::Matrix4f _worldMatrix{Eigen::Matrix4f::Identity()};
};

}  // namespace radray
