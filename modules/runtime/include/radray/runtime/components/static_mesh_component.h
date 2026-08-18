#pragma once

#include <span>

#include <radray/nullable.h>
#include <radray/runtime/asset_manager.h>
#include <radray/runtime/components/primitive_component.h>
#include <radray/runtime/static_mesh.h>
#include <radray/types.h>

namespace radray {

class Material;

class StaticMeshComponent final : public PrimitiveComponent {
public:
    StaticMeshComponent() noexcept = default;
    ~StaticMeshComponent() noexcept override;

    RuntimeTypeId GetTypeId() const noexcept override;

    void SetStaticMesh(StreamingAssetRef<StaticMesh> mesh);
    const StreamingAssetRef<StaticMesh>& GetStaticMesh() const noexcept { return _mesh; }

    void SetMaterial(uint32_t sectionIndex, Nullable<Material*> material);
    Nullable<Material*> GetMaterial(uint32_t sectionIndex) const noexcept;
    std::span<const Nullable<Material*>> GetMaterials() const noexcept { return _materials; }

    bool ShouldCreateRenderState() const override;
    unique_ptr<PrimitiveSceneProxy> CreateSceneProxy() override;

private:
    StreamingAssetRef<StaticMesh> _mesh;
    vector<Nullable<Material*>> _materials;
};

template <>
struct RuntimeTypeTrait<StaticMeshComponent> {
    static constexpr RuntimeTypeId value{0x7911a3cb, 0x45a7, 0x46d8, 0xa3, 0xb1, 0x71, 0xc4, 0xf7, 0x85, 0x32, 0x0e};
    using Bases = std::tuple<PrimitiveComponent>;
};

}  // namespace radray
