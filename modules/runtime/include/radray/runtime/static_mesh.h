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

/// Shared, immutable geometry while its owning StaticMesh asset is retained.
struct StaticMeshRenderData {
    GpuMesh Mesh;
    vector<StaticMeshSection> Sections;
    Eigen::Vector3f LocalBoundsMin{Eigen::Vector3f::Zero()};
    Eigen::Vector3f LocalBoundsMax{Eigen::Vector3f::Zero()};
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
    const vector<StaticMeshSection>& GetSections() const noexcept { return _renderData.Sections; }
    const Eigen::Vector3f& GetBoundsMin() const noexcept { return _renderData.LocalBoundsMin; }
    const Eigen::Vector3f& GetBoundsMax() const noexcept { return _renderData.LocalBoundsMax; }

    bool IsValid() const noexcept;

    /// 借用资源；Scene 外的提交者须用 GpuSystem::RetainForFrameGT 保持资产至实际完成。
    const StaticMeshRenderData& GetRenderData() const noexcept { return _renderData; }
    const GpuMesh& GetRenderMesh() const noexcept { return _renderData.Mesh; }

private:
    MeshResource _meshResource;
    StaticMeshRenderData _renderData;
    bool _valid{false};
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
