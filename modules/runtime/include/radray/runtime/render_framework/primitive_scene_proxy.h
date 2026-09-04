#pragma once

#include <radray/basic_math.h>
#include <radray/nullable.h>
#include <radray/runtime/gpu_resource.h>
#include <radray/types.h>

// 基本体侧代理。场景数据如何从 Actor/Component 流到渲染侧:
// docs/architecture/render-framework.md

namespace radray {

class Material;
class StreamingAssetRefAny;

/// 一次索引绘制的参数 (对应 UE5 的 FMeshBatchElement 的索引子集)。
/// 【Geometry 的保命责任在 proxy】它指进 StaticMesh 持有的 GpuMesh, 所以覆写 GetDrawArgs 的
/// proxy 必须自己存一份 StreamingAssetRef<StaticMesh>。
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

    /// Game thread: retain every asset owning geometry exposed by this proxy.
    virtual void CollectAssetReferences(vector<StreamingAssetRefAny>& out) const;

    uint64_t GetGeneration() const noexcept { return _generation; }

    /// 逐物体 local->world 变换 (对应 UE5 的 GetLocalToWorld / Unity 的 unity_ObjectToWorld)。
    /// 基类默认单位阵; 具体 proxy 覆写。
    virtual Eigen::Matrix4f GetLocalToWorld() const noexcept { return Eigen::Matrix4f::Identity(); }

    /// 取指定 section 的绘制参数 (几何 + 索引范围)。执行器据此绑定 VB/IB 并 DrawIndexed。
    /// 基类默认无几何 (Geometry=nullptr); 具体 proxy 覆写。
    virtual MeshDrawArgs GetDrawArgs(uint32_t /*sectionIndex*/) const noexcept { return MeshDrawArgs{}; }
    virtual uint32_t GetSectionCount() const noexcept { return 0; }
    virtual Nullable<Material*> GetMaterial(uint32_t /*sectionIndex*/) const noexcept { return nullptr; }

private:
    uint64_t _generation{0};
};

}  // namespace radray
