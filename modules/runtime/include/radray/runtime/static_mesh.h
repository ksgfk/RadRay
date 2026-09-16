#pragma once

#include <filesystem>
#include <span>

#include <radray/vertex_data.h>
#include <radray/runtime/asset.h>
#include <radray/runtime/asset_database.h>
#include <radray/runtime/asset_manager.h>
#include <radray/runtime/gpu_resource.h>

namespace radray {

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
/// 构造方必须提供完整 CPU/GPU 数据；内置 GPU 上传加载路径暂未实现。
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

    const MeshResource& GetMeshResource() const noexcept { return _meshResource; }
    const vector<StaticMeshSection>& GetSections() const noexcept { return _sections; }
    const Eigen::Vector3f& GetBoundsMin() const noexcept { return _boundsMin; }
    const Eigen::Vector3f& GetBoundsMax() const noexcept { return _boundsMax; }

    bool IsValid() const noexcept;

    // ─── GPU 渲染数据 ───
    // 对应 UE5 的 FStaticMeshRenderData: 上传后的 device-local 顶点/索引 buffer。
    // 返回指针在【本资产】存活期内稳定 —— 持有一份 StreamingAssetRef 即保证不悬垂

    const GpuMesh& GetRenderMesh() const noexcept { return _renderMesh; }

private:
    MeshResource _meshResource;
    vector<StaticMeshSection> _sections;
    Eigen::Vector3f _boundsMin;
    Eigen::Vector3f _boundsMax;
    GpuMesh _renderMesh;
};

class MeshImporter final : public AssetImporter {
public:
    std::string_view GetTypeName() const noexcept override;
    std::span<const std::string_view> GetFileExtensions() const noexcept override;
    task<AssetLoadResult> Load(const AssetLoadContext& ctx) override;

private:
    static task<AssetLoadResult> LoadMesh(
        std::filesystem::path path);
};

template <>
struct RuntimeTypeTrait<StaticMesh> {
    static constexpr RuntimeTypeId value{0x9226f085, 0xb0b1, 0x476f, 0xb7, 0x29, 0x69, 0xec, 0xee, 0x38, 0x99, 0x8c};
};

}  // namespace radray
