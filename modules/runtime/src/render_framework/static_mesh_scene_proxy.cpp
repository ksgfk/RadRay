#include <radray/runtime/render_framework/static_mesh_scene_proxy.h>

#include <utility>

namespace radray {

StaticMeshSceneProxy::StaticMeshSceneProxy(
    StreamingAssetRef<StaticMesh> mesh,
    vector<Nullable<Material*>> materials,
    const Eigen::Matrix4f& localToWorld) noexcept
    : _mesh(std::move(mesh)),
      _materials(std::move(materials)),
      _localToWorld(localToWorld) {}

StaticMeshSceneProxy::~StaticMeshSceneProxy() noexcept = default;

void StaticMeshSceneProxy::CollectAssetReferences(vector<StreamingAssetRefAny>& out) const {
    out.push_back(_mesh.AsAny());
}

MeshDrawArgs StaticMeshSceneProxy::GetDrawArgs(uint32_t sectionIndex) const noexcept {
    const StaticMesh* mesh = _mesh.Get();
    if (mesh == nullptr || sectionIndex >= mesh->GetSections().size()) {
        return {};
    }
    const StaticMeshSection& section = mesh->GetSections()[sectionIndex];
    if (section.PrimitiveIndex >= mesh->GetRenderMesh().Draws.size()) {
        return {};
    }
    return MeshDrawArgs{
        .Geometry = &mesh->GetRenderMesh().Draws[section.PrimitiveIndex],
        .FirstIndex = section.FirstIndex,
        .IndexCount = section.IndexCount,
        .VertexOffset = section.VertexOffset};
}

uint32_t StaticMeshSceneProxy::GetSectionCount() const noexcept {
    const StaticMesh* mesh = _mesh.Get();
    return mesh != nullptr ? static_cast<uint32_t>(mesh->GetSections().size()) : 0;
}

Nullable<Material*> StaticMeshSceneProxy::GetMaterial(uint32_t sectionIndex) const noexcept {
    return sectionIndex < _materials.size() ? _materials[sectionIndex] : nullptr;
}

}  // namespace radray
