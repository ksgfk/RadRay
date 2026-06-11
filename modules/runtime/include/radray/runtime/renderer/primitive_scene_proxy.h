#pragma once

#include <radray/basic_math.h>
#include <radray/types.h>
#include <radray/render/common.h>

namespace radray::render {
class CommandBuffer;
}

namespace radray {

class Material;
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

    /// 代理的资源生命周期阶段。对应 UE5 FRenderResource 的初始化状态(最小化)。
    /// 数据驱动:渲染系统每帧遍历代理,对 Pending 态调用 UpdateResources 准备 GPU 资源。
    enum class ResourceState {
        Pending,    ///< GPU 资源未就绪:需在帧顶 UpdateResources 上传/构建。代理新建时的初态。
        Uploading,  ///< (预留)异步传输队列上传中,跨帧未就绪。当前单队列模式不进入此态。
        Ready,      ///< 资源就绪:可参与可见性收集与绘制。
    };

    /// 获取世界变换矩阵
    const Eigen::Matrix4f& GetWorldMatrix() const noexcept { return _worldMatrix; }
    void SetWorldMatrix(const Eigen::Matrix4f& matrix) noexcept { _worldMatrix = matrix; }

    /// 当前资源生命周期阶段。
    ResourceState GetResourceState() const noexcept { return _resourceState; }

    /// 帧顶资源准备:在已 Begin、RenderPass 之前的裸 CommandBuffer 上录制资源初始化/
    /// 上传(copy)命令,并推进生命周期状态。由 SceneRenderer::PrepareResources 仅在
    /// Pending 态调用。默认实现:无 GPU 资源需求,直接标记 Ready。
    /// 对应 UE5 渲染线程驱动的 FRenderResource::InitResource。
    virtual void UpdateResources(render::CommandBuffer* cmdBuffer, ResourceUploader& uploader) {
        (void)cmdBuffer;
        (void)uploader;
        _resourceState = ResourceState::Ready;
    }

    /// 是否可被渲染(GPU 数据就绪)。默认 false,派生类按需覆写。
    virtual bool IsRenderable() const noexcept { return false; }

    /// 收集本代理的几何绘制单元。MeshPassProcessor 据此 + 材质 + GetWorldMatrix()
    /// 解析出完整 MeshDrawCommand。对应 UE5 FPrimitiveSceneProxy::DrawStaticElements。
    /// 追加而非清空 out,便于一次性收集全场景。
    virtual void CollectBatchElements(vector<MeshBatchElement>& out) const { (void)out; }

    /// 本代理关联的材质(决定 shader/PSO/渲染状态)。默认无材质。
    virtual Material* GetMaterial() const noexcept { return nullptr; }

    /// 本代理几何对应的顶点布局(供 PSO input layout)。默认空布局。
    virtual const render::VertexBufferLayout& GetVertexLayout() const noexcept {
        static const render::VertexBufferLayout kEmpty{};
        return kEmpty;
    }

protected:
    void SetResourceState(ResourceState state) noexcept { _resourceState = state; }

private:
    Eigen::Matrix4f _worldMatrix{Eigen::Matrix4f::Identity()};
    ResourceState _resourceState{ResourceState::Pending};
};

}  // namespace radray
