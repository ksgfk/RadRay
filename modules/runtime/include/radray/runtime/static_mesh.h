#pragma once

#include <filesystem>
#include <span>

#include <radray/vertex_data.h>
#include <radray/runtime/asset.h>
#include <radray/runtime/asset_database.h>
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
/// 【为何是自由函数】: 上传前就要校验, 那时还没有任何资产对象。从前的写法是构造一个空
/// StaticMesh 当 probe 再 SetMeshResource, 而资产不可变之后 probe 无从存在 —— 校验本就
/// 只依赖数据, 不该依赖资产。
bool IsStaticMeshDataValid(
    const MeshResource& meshResource,
    std::span<const StaticMeshSection> sections) noexcept;

/// 静态网格资产。CPU 网格数据 + section/bounds + 已上传的 GPU 渲染数据。
///
/// 【构造即完整】: CPU 数据与 GPU 上传都由加载协程在构造前备齐, 资产一出生即可渲染,
/// 不再有二段式回填 (从前的 SetSections / SetBounds)。
class StaticMesh : public Asset {
public:
    StaticMesh(
        MeshResource meshResource,
        vector<StaticMeshSection> sections,
        const Eigen::Vector3f& boundsMin,
        const Eigen::Vector3f& boundsMax,
        GpuMesh renderMesh) noexcept;
    ~StaticMesh() noexcept override;

    void OnUnload(AssetManager& manager) override;
    RuntimeTypeId GetTypeId() const noexcept override;

    const MeshResource& GetMeshResource() const noexcept { return _meshResource; }
    const vector<StaticMeshSection>& GetSections() const noexcept { return _sections; }
    const Eigen::Vector3f& GetBoundsMin() const noexcept { return _boundsMin; }
    const Eigen::Vector3f& GetBoundsMax() const noexcept { return _boundsMax; }

    bool IsValid() const noexcept;

    // ─── GPU 渲染数据 ───
    // 对应 UE5 的 FStaticMeshRenderData: 上传后的 device-local 顶点/索引 buffer。
    // 返回指针在【本资产】存活期内稳定 —— 持有一份 StreamingAssetRef 即保证不悬垂
    // (SceneProxy 缓存的 DrawData* 正是靠它, 见 primitive_scene_proxy.h)。

    const GpuMesh& GetRenderMesh() const noexcept { return _renderMesh; }

private:
    MeshResource _meshResource;
    vector<StaticMeshSection> _sections;
    Eigen::Vector3f _boundsMin;
    Eigen::Vector3f _boundsMax;
    GpuMesh _renderMesh;
};

/// StaticMesh 的异步加载工厂。参数为已构建好的 CPU 网格数据(MeshResource);
/// 协程内部完成 GPU 上传(跨帧),上传完成后一次性构造资产。
/// 加载阶段对 AssetManager 不可见(协程内部事务)。
task<AssetLoadResult> LoadStaticMesh(
    FrameUploadScheduler& frameUploads,
    MeshResource meshResource);

class MeshImporter final : public AssetImporter {
public:
    explicit MeshImporter(FrameUploadScheduler& frameUploads) noexcept;

    std::string_view GetTypeName() const noexcept override;
    std::span<const std::string_view> GetFileExtensions() const noexcept override;
    task<AssetLoadResult> Load(const AssetLoadContext& ctx) override;

private:
    static task<AssetLoadResult> LoadMesh(
        FrameUploadScheduler* frameUploads,
        std::filesystem::path path);

    FrameUploadScheduler& _frameUploads;
};

template <>
struct RuntimeTypeTrait<StaticMesh> {
    static constexpr RuntimeTypeId value{0x9226f085, 0xb0b1, 0x476f, 0xb7, 0x29, 0x69, 0xec, 0xee, 0x38, 0x99, 0x8c};
    using Bases = std::tuple<Asset>;
};

}  // namespace radray
