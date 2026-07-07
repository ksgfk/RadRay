#include <radray/runtime/render_framework/static_mesh_scene_proxy.h>

#include <radray/runtime/components/static_mesh_component.h>
#include <radray/runtime/material_asset.h>

namespace radray {
namespace {

vector<StaticMeshSceneProxy::Section> BuildSections(const StaticMesh& mesh) {
    vector<StaticMeshSceneProxy::Section> sections;
    sections.reserve(mesh.GetSections().size());

    for (const StaticMeshSection& section : mesh.GetSections()) {
        sections.push_back(StaticMeshSceneProxy::Section{
            .PrimitiveIndex = section.PrimitiveIndex,
            .FirstIndex = section.FirstIndex,
            .IndexCount = section.IndexCount,
            .MinVertexIndex = section.MinVertexIndex,
            .MaxVertexIndex = section.MaxVertexIndex,
        });
    }

    return sections;
}

}  // namespace

StaticMeshSceneProxy::StaticMeshSceneProxy(const StaticMeshComponent& component, StreamingAssetRef<StaticMesh> mesh)
    : _mesh(std::move(mesh)),
      _localToWorld(component.GetWorldMatrix()),
      _localBoundsMin(_mesh.Get() != nullptr ? _mesh->GetBoundsMin() : Eigen::Vector3f::Zero()),
      _localBoundsMax(_mesh.Get() != nullptr ? _mesh->GetBoundsMax() : Eigen::Vector3f::Zero()),
      _sections(_mesh.Get() != nullptr ? BuildSections(*_mesh.Get()) : vector<Section>{}) {
    _sectionSnapshots.reserve(_sections.size());
    for (size_t i = 0; i < _sections.size(); ++i) {
        _sectionSnapshots.emplace_back(make_unique<AtomicSnapshot>());
    }
}

StaticMeshSceneProxy::~StaticMeshSceneProxy() noexcept = default;

const render::RenderMesh* StaticMeshSceneProxy::GetRenderMesh() const noexcept {
    StaticMesh* mesh = _mesh.Get();
    if (mesh == nullptr) {
        return nullptr;
    }
    return mesh->GetRenderMesh();
}

void StaticMeshSceneProxy::SetSectionSnapshot(
    uint32_t sectionIndex,
    shared_ptr<const MaterialRenderSnapshot> snapshot) noexcept {
    if (sectionIndex >= _sectionSnapshots.size()) {
        return;
    }
    _sectionSnapshots[sectionIndex]->store(std::move(snapshot), std::memory_order_release);
}

MeshDrawArgs StaticMeshSceneProxy::GetDrawArgs(uint32_t sectionIndex) const noexcept {
    if (sectionIndex >= _sections.size()) {
        return MeshDrawArgs{};
    }
    const render::RenderMesh* mesh = GetRenderMesh();
    if (mesh == nullptr) {
        return MeshDrawArgs{};
    }
    const Section& sec = _sections[sectionIndex];
    if (sec.PrimitiveIndex >= mesh->_drawDatas.size()) {
        return MeshDrawArgs{};
    }
    MeshDrawArgs args{};
    args.Geometry = &mesh->_drawDatas[sec.PrimitiveIndex];
    args.FirstIndex = sec.FirstIndex;
    args.IndexCount = sec.IndexCount;
    args.VertexOffset = 0;
    return args;
}

}  // namespace radray
