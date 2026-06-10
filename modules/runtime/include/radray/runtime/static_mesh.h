#pragma once

#include <radray/vertex_data.h>
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

    void OnLoad() override;
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

private:
    MeshResource _meshResource;
    vector<StaticMeshSection> _sections;
    Eigen::Vector3f _boundsMin;
    Eigen::Vector3f _boundsMax;
};

template <>
struct AssetTypeTrait<StaticMesh> {
    static constexpr AssetTypeId value{0x9226f085, 0xb0b1, 0x476f, 0xb7, 0x29, 0x69, 0xec, 0xee, 0x38, 0x99, 0x8c};
};

}  // namespace radray
