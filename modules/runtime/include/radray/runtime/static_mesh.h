#pragma once

#include <radray/vertex_data.h>
#include <radray/runtime/asset.h>
#include <radray/runtime/asset_manager.h>
#include <radray/runtime/gpu_resource.h>

namespace radray {

class FrameUploadScheduler;

struct StaticMeshSection {
    StaticMeshSection() noexcept;
    StaticMeshSection(
        uint32_t primitiveIndex,
        uint32_t firstIndex,
        uint32_t indexCount,
        uint32_t minVertexIndex,
        uint32_t maxVertexIndex,
        int32_t vertexOffset = 0) noexcept;

    uint32_t PrimitiveIndex;
    uint32_t FirstIndex;
    uint32_t IndexCount;
    uint32_t MinVertexIndex;
    uint32_t MaxVertexIndex;
    int32_t VertexOffset;
};

class StaticMesh : public Asset {
public:
    StaticMesh() noexcept;
    /// 构造即完整:CPU 网格数据 + 已上传的 GPU 渲染数据一次性交付。
    /// 由加载协程在 GPU 上传完成后调用。资产入库即处于可渲染状态,不再有二段式回填。
    StaticMesh(MeshResource meshResource, shared_ptr<GpuMesh> renderMesh) noexcept;
    ~StaticMesh() noexcept override;

    void OnUnload(IRenderResourceRecycler& recycler) override;
    AssetTypeId GetTypeId() const noexcept override;

    MeshResource& GetMeshResource() noexcept;
    const MeshResource& GetMeshResource() const noexcept;
    void SetMeshResource(MeshResource meshResource);

    vector<StaticMeshSection>& GetSections() noexcept;
    const vector<StaticMeshSection>& GetSections() const noexcept;
    void SetSections(vector<StaticMeshSection> sections);

    const Eigen::Vector3f& GetBoundsMin() const noexcept;
    const Eigen::Vector3f& GetBoundsMax() const noexcept;
    void SetBounds(const Eigen::Vector3f& boundsMin, const Eigen::Vector3f& boundsMax) noexcept;

    bool IsValid() const noexcept;
    void ClearCPUData() noexcept;

    // ─── GPU 渲染数据 ───
    // 对应 UE5 的 FStaticMeshRenderData:持有上传后的 device-local 顶点/索引 buffer。
    // 加载协程上传完成后通过构造函数交给资产持有,资产释放(OnUnload)时一并销毁。

    bool HasRenderData() const noexcept { return _renderMesh != nullptr; }
    GpuMesh* GetRenderMesh() noexcept { return _renderMesh.get(); }
    const GpuMesh* GetRenderMesh() const noexcept { return _renderMesh.get(); }
    void ClearRenderData() noexcept { _renderMesh.reset(); }

private:
    MeshResource _meshResource;
    vector<StaticMeshSection> _sections;
    Eigen::Vector3f _boundsMin;
    Eigen::Vector3f _boundsMax;
    shared_ptr<GpuMesh> _renderMesh;
};

/// StaticMesh 的异步加载工厂。参数为已构建好的 CPU 网格数据(MeshResource);
/// 协程内部完成 GPU 上传(跨帧),上传完成后一次性构造 StaticMesh。
/// 加载阶段对 AssetManager 不可见(协程内部事务)。
AssetLoadTask LoadStaticMesh(FrameUploadScheduler& frameUploads, MeshResource meshResource);

template <>
struct RuntimeTypeTrait<StaticMesh> {
    static constexpr RuntimeTypeId value{0x9226f085, 0xb0b1, 0x476f, 0xb7, 0x29, 0x69, 0xec, 0xee, 0x38, 0x99, 0x8c};
    using Bases = std::tuple<Asset>;
};

}  // namespace radray
