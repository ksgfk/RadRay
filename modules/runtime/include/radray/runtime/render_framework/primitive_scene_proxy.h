#pragma once

#include <radray/basic_math.h>
#include <radray/runtime/gpu_resource.h>
#include <radray/types.h>

namespace radray {

/// 一次索引绘制的参数 (对应 UE5 的 FMeshBatchElement 的索引子集)。
///
/// 【Geometry 的保命责任在 proxy】: 它指进 StaticMesh 持有的 GpuMesh。覆写 GetDrawArgs 的
/// proxy 必须自己存一份 StreamingAssetRef<StaticMesh> —— 引用计数是资产生命周期的唯一
/// 权威, 持有它即保证这块 GpuMesh 不被销毁 (且归零后的 GPU buffer 还要过一次帧边界才真死,
/// 见 StaticMesh::OnUnload)。见 asset.h。
struct MeshDrawArgs {
    const GpuMesh::DrawData* Geometry{nullptr};  // VB/IB 视图
    uint32_t FirstIndex{0};
    uint32_t IndexCount{0};
    int32_t VertexOffset{0};
};

/// 渲染基本体组件的侧代理。
/// 对应UE5的FPrimitiveSceneProxy。
class PrimitiveSceneProxy {
public:
    PrimitiveSceneProxy() noexcept;
    PrimitiveSceneProxy(const PrimitiveSceneProxy&) = delete;
    PrimitiveSceneProxy(PrimitiveSceneProxy&&) = delete;
    PrimitiveSceneProxy& operator=(const PrimitiveSceneProxy&) = delete;
    PrimitiveSceneProxy& operator=(PrimitiveSceneProxy&&) = delete;
    virtual ~PrimitiveSceneProxy() noexcept;

    uint64_t GetGeneration() const noexcept { return _generation; }

    /// 逐物体 local->world 变换 (对应 UE5 的 GetLocalToWorld / Unity 的 unity_ObjectToWorld)。
    /// 基类默认单位阵; 具体 proxy 覆写。
    virtual Eigen::Matrix4f GetLocalToWorld() const noexcept { return Eigen::Matrix4f::Identity(); }

    /// 取指定 section 的绘制参数 (几何 + 索引范围)。执行器据此绑定 VB/IB 并 DrawIndexed。
    /// 基类默认无几何 (Geometry=nullptr); 具体 proxy 覆写。
    virtual MeshDrawArgs GetDrawArgs(uint32_t /*sectionIndex*/) const noexcept { return MeshDrawArgs{}; }

private:
    uint64_t _generation{0};
};

}  // namespace radray
