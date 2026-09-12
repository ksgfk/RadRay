#include <radray/runtime/render_framework/static_mesh_scene_proxy.h>

#include <utility>

namespace radray {

StaticMeshSceneProxy::StaticMeshSceneProxy(
    StreamingAssetRef<StaticMesh> mesh,
    vector<Nullable<Material*>> materials,
    const Eigen::Matrix4f& localToWorld) noexcept
    : _mesh(std::move(mesh)),
      _materials(std::move(materials)) {
    SetLocalToWorld(localToWorld);
}

StaticMeshSceneProxy::~StaticMeshSceneProxy() noexcept = default;

uint64_t StaticMeshSceneProxy::GetRenderDataRevision() const noexcept {
    return (_renderDataRevision << 1) | static_cast<uint64_t>(_mesh.IsReady());
}

bool StaticMeshSceneProxy::HasPendingRenderResources() const noexcept {
    return _mesh.IsValid() && !_mesh.IsCompleted();
}

void StaticMeshSceneProxy::SetStaticMesh(StreamingAssetRef<StaticMesh> mesh) noexcept {
    if (_mesh == mesh) return;
    if (_renderDataRevision == (UINT64_MAX >> 1)) RADRAY_ABORT("Static mesh revision exhausted");
    _mesh = std::move(mesh);
    ++_renderDataRevision;
    MarkRenderDirty(PrimitiveDirtyKind::Structure | PrimitiveDirtyKind::TransformOrBounds);
    ResetMotion();
}

void StaticMeshSceneProxy::SetMaterial(uint32_t sectionIndex, Nullable<Material*> material) {
    if (sectionIndex < _materials.size() && _materials[sectionIndex] == material) return;
    if (sectionIndex >= _materials.size()) _materials.resize(static_cast<size_t>(sectionIndex) + 1);
    _materials[sectionIndex] = material;
    MarkRenderDirty(PrimitiveDirtyKind::MaterialAssignment);
}

AxisAlignedBounds StaticMeshSceneProxy::GetLocalBounds() const noexcept {
    const auto mesh = _mesh.Get();
    return mesh ? AxisAlignedBounds{mesh->GetBoundsMin(), mesh->GetBoundsMax()} : AxisAlignedBounds{};
}

void StaticMeshSceneProxy::CollectAssetReferences(vector<StreamingAssetRefAny>& out) const {
    out.push_back(_mesh.AsAny());
}

MeshDrawArgs StaticMeshSceneProxy::GetDrawArgs(uint32_t sectionIndex) const noexcept {
    Nullable<StaticMesh*> mesh = _mesh.Get();
    if (!mesh || sectionIndex >= mesh->GetSections().size()) {
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
    Nullable<StaticMesh*> mesh = _mesh.Get();
    return mesh ? static_cast<uint32_t>(mesh->GetSections().size()) : 0;
}

Nullable<Material*> StaticMeshSceneProxy::GetMaterial(uint32_t sectionIndex) const noexcept {
    return sectionIndex < _materials.size() ? _materials[sectionIndex] : nullptr;
}

}  // namespace radray
