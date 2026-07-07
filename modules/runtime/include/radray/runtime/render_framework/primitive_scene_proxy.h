#pragma once

#include <cstdint>

#include <radray/basic_math.h>
#include <radray/render/gpu_resource.h>
#include <radray/types.h>

namespace radray {

class MaterialAsset;
struct MaterialRenderSnapshot;

/// 一次索引绘制的参数 (对应 UE5 的 FMeshBatchElement 的索引子集)。
struct MeshDrawArgs {
    const render::RenderMesh::DrawData* Geometry{nullptr};  // VB/IB view
    uint32_t FirstIndex{0};
    uint32_t IndexCount{0};
    int32_t VertexOffset{0};
};

/// Render-side proxy for a primitive component.
/// Corresponds to UE5's FPrimitiveSceneProxy.
class PrimitiveSceneProxy {
public:
    PrimitiveSceneProxy() noexcept = default;
    PrimitiveSceneProxy(const PrimitiveSceneProxy&) = delete;
    PrimitiveSceneProxy(PrimitiveSceneProxy&&) = delete;
    PrimitiveSceneProxy& operator=(const PrimitiveSceneProxy&) = delete;
    PrimitiveSceneProxy& operator=(PrimitiveSceneProxy&&) = delete;
    virtual ~PrimitiveSceneProxy() noexcept;

    /// 逐物体 local->world 变换 (对应 UE5 的 GetLocalToWorld / Unity 的 unity_ObjectToWorld)。
    /// 基类默认单位阵; 具体 proxy 覆写。
    virtual Eigen::Matrix4f GetLocalToWorld() const noexcept {
        return Eigen::Matrix4f::Identity();
    }

    /// 取指定 section 的绘制参数 (几何 + 索引范围)。执行器据此绑定 VB/IB 并 DrawIndexed。
    /// 基类默认无几何 (Geometry=nullptr); 具体 proxy 覆写。
    virtual MeshDrawArgs GetDrawArgs(uint32_t /*sectionIndex*/) const noexcept {
        return MeshDrawArgs{};
    }

    /// section (sub-mesh) 数量。基类默认 0; 具体 proxy 覆写。
    /// RTTI 被禁用 (/GR-), 故用虚接口而非 dynamic_cast 遍历 section。
    virtual uint32_t GetSectionCount() const noexcept {
        return 0;
    }

    /// 取指定 section 的材质渲染快照 (只读值快照, 无锁跨线程发布)。
    /// 基类默认空; 具体 proxy 覆写。render 线程据此构建 DrawList / 提交绘制,
    /// 不再回查 game 线程的 MaterialAsset (避免竞争与 Unload 悬垂)。
    virtual shared_ptr<const MaterialRenderSnapshot> GetSectionSnapshot(uint32_t /*sectionIndex*/) const noexcept {
        return nullptr;
    }
};

}  // namespace radray
