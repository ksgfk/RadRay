#pragma once

#include <optional>

#include <radray/vertex_data.h>
#include <radray/render/gpu_resource.h>
#include <radray/runtime/asset.h>

namespace radray {

struct StaticMeshSection {
    StaticMeshSection() noexcept;
    StaticMeshSection(
        uint32_t primitiveIndex,
        uint32_t firstIndex,
        uint32_t indexCount,
        uint32_t minVertexIndex,
        uint32_t maxVertexIndex) noexcept;

    uint32_t PrimitiveIndex;
    uint32_t FirstIndex;
    uint32_t IndexCount;
    uint32_t MinVertexIndex;
    uint32_t MaxVertexIndex;
};

class StaticMesh : public Asset {
public:
    StaticMesh() noexcept;
    ~StaticMesh() noexcept override;

    void OnUnload() override;
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
    // 由 ResourceUploader::UploadMesh 产出后通过 SetRenderMesh 交给资产持有,
    // 资产释放(OnUnload)时一并销毁。多个组件可共享同一份 GPU 数据。

    bool HasRenderData() const noexcept { return _renderMesh.has_value(); }
    render::RenderMesh* GetRenderMesh() noexcept { return _renderMesh ? &_renderMesh.value() : nullptr; }
    const render::RenderMesh* GetRenderMesh() const noexcept { return _renderMesh ? &_renderMesh.value() : nullptr; }
    void SetRenderMesh(render::RenderMesh renderMesh) { _renderMesh = std::move(renderMesh); }
    void ClearRenderData() noexcept { _renderMesh.reset(); }

private:
    MeshResource _meshResource;
    vector<StaticMeshSection> _sections;
    Eigen::Vector3f _boundsMin;
    Eigen::Vector3f _boundsMax;
    std::optional<render::RenderMesh> _renderMesh;
};

template <>
struct AssetTypeTrait<StaticMesh> {
    static constexpr AssetTypeId value{0x9226f085, 0xb0b1, 0x476f, 0xb7, 0x29, 0x69, 0xec, 0xee, 0x38, 0x99, 0x8c};
};

}  // namespace radray
