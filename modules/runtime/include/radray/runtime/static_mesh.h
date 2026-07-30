#pragma once

#include <span>

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

/// CPU 网格数据的自洽性校验。section 为空时只校验 primitive。
///
/// 【为何是自由函数】: 上传前就要校验, 那时还没有任何内容对象。从前的写法是构造一个空
/// StaticMesh 当 probe 再 SetMeshResource, 而内容不可变之后 probe 无从存在 —— 校验本就
/// 只依赖数据, 不该依赖资产。
bool IsStaticMeshDataValid(
    const MeshResource& meshResource,
    std::span<const StaticMeshSection> sections) noexcept;

/// 静态网格的【内容】。CPU 网格数据 + section/bounds + 已上传的 GPU 渲染数据。
///
/// 【为何网格需要内容/槽位分离】: SceneProxy 存的是 GpuMesh::DrawData* 裸指针 (见
/// primitive_scene_proxy.h 的 Geometry 字段), 录进命令列表后要活到 fence。分离前
/// StaticMesh 用 shared_ptr<GpuMesh> 自己做了一半 —— 但 OnUnload 里那句
/// use_count() == 1 使"别人还持有"这条分支绕过 recycler, 恰恰是唯一有 GPU 危险的分支
/// (见 asset.h)。归零动作交给 AssetContentDeleter 后这个洞自然消失。
///
/// 【构造即完整】: CPU 数据与 GPU 上传都由加载协程在构造前备齐, 内容一出生即可渲染,
/// 不再有二段式回填 (从前的 SetSections / SetBounds)。
class StaticMeshContent {
public:
    /// 【recycler 只收不存】: 归零时的释放由 AssetContentDeleter 完成, 它自己持有 recycler
    /// (见 asset.h)。这里保留形参是因为 MakeContent 统一把 GetRecycler() 作第二实参转发,
    /// 内容类型自身不再需要它。
    StaticMeshContent(
        AssetContentKey key,
        IRenderResourceRecycler& recycler,
        MeshResource meshResource,
        vector<StaticMeshSection> sections,
        const Eigen::Vector3f& boundsMin,
        const Eigen::Vector3f& boundsMax,
        GpuMesh renderMesh) noexcept;
    StaticMeshContent(const StaticMeshContent&) = delete;
    StaticMeshContent(StaticMeshContent&&) = delete;
    StaticMeshContent& operator=(const StaticMeshContent&) = delete;
    StaticMeshContent& operator=(StaticMeshContent&&) = delete;
    ~StaticMeshContent() noexcept;

    const MeshResource& GetMeshResource() const noexcept { return _meshResource; }
    const vector<StaticMeshSection>& GetSections() const noexcept { return _sections; }
    const Eigen::Vector3f& GetBoundsMin() const noexcept { return _boundsMin; }
    const Eigen::Vector3f& GetBoundsMax() const noexcept { return _boundsMax; }

    bool IsValid() const noexcept;

    // ─── GPU 渲染数据 ───
    // 对应 UE5 的 FStaticMeshRenderData: 上传后的 device-local 顶点/索引 buffer。
    // 返回指针在【本内容】存活期内稳定 —— 持有一份 shared_ptr 即保证不悬垂。

    const GpuMesh& GetRenderMesh() const noexcept { return _renderMesh; }

    /// 【只由 AssetContentDeleter 在引用归零时调用, 普通代码不得调用】: 它跑在析构【之前】,
    /// 故此刻成员仍然完整; 提前调用会留下一个成员已被搬空的内容对象。见 asset.h。
    void ReleaseRenderResources(IRenderResourceRecycler& recycler) noexcept;

private:
    MeshResource _meshResource;
    vector<StaticMeshSection> _sections;
    Eigen::Vector3f _boundsMin;
    Eigen::Vector3f _boundsMax;
    GpuMesh _renderMesh;
};

/// 静态网格资产。只做标识与槽位, 数据在 StaticMeshContent。
class StaticMesh : public Asset {
public:
    explicit StaticMesh(shared_ptr<StaticMeshContent> content) noexcept;
    ~StaticMesh() noexcept override;

    void OnUnload(IRenderResourceRecycler& recycler) override;
    AssetTypeId GetTypeId() const noexcept override;

    /// 取内容的强引用。持有它期间内容保证存活, 即使本资产的槽位已被 Unload。
    ///
    /// 【刻意不提供 GetRenderMesh / GetSections 等转发】: 见 Asset 的说明。SceneProxy 应当
    /// 存住一份内容引用 —— 它缓存的 DrawData* 正是靠这份引用才不悬垂。
    shared_ptr<StaticMeshContent> AcquireContent() const noexcept { return _content; }

    /// 内容是否仍挂在本槽位上。OnUnload 之后为 false。
    bool HasContent() const noexcept { return _content != nullptr; }

private:
    shared_ptr<StaticMeshContent> _content;
};

/// StaticMesh 的异步加载工厂。参数为已构建好的 CPU 网格数据(MeshResource);
/// 协程内部完成 GPU 上传(跨帧),上传完成后一次性构造内容与资产。
/// 加载阶段对 AssetManager 不可见(协程内部事务)。
///
/// 【为何收 AssetManager&】: StaticMeshContent 只能经 AssetManager::MakeContent 创建
/// (recycler 由那里注入), 见 AssetContentKey。
AssetLoadTask LoadStaticMesh(
    AssetManager& assetManager,
    FrameUploadScheduler& frameUploads,
    MeshResource meshResource);

template <>
struct RuntimeTypeTrait<StaticMesh> {
    static constexpr RuntimeTypeId value{0x9226f085, 0xb0b1, 0x476f, 0xb7, 0x29, 0x69, 0xec, 0xee, 0x38, 0x99, 0x8c};
    using Bases = std::tuple<Asset>;
};

}  // namespace radray
