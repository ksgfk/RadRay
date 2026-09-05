#include <radray/runtime/components/static_mesh_component.h>

#include <utility>

#include <radray/runtime/render_framework/static_mesh_scene_proxy.h>

namespace radray {

StaticMeshComponent::~StaticMeshComponent() noexcept = default;

void StaticMeshComponent::SetStaticMesh(StreamingAssetRef<StaticMesh> mesh) {
    if (_mesh == mesh) {
        return;
    }
    _mesh = std::move(mesh);
    MarkRenderStateDirty();
}

void StaticMeshComponent::SetMaterial(
    uint32_t sectionIndex,
    Nullable<Material*> material) {
    if (sectionIndex >= _materials.size()) {
        _materials.resize(static_cast<size_t>(sectionIndex) + 1);
    }
    if (_materials[sectionIndex].Get() == material.Get()) {
        return;
    }
    _materials[sectionIndex] = material;
    MarkRenderStateDirty();
}

Nullable<Material*> StaticMeshComponent::GetMaterial(uint32_t sectionIndex) const noexcept {
    return sectionIndex < _materials.size() ? _materials[sectionIndex] : nullptr;
}

bool StaticMeshComponent::ShouldCreateRenderState() const {
    return _mesh.IsReady() && _mesh.Get() != nullptr && _mesh->IsValid();
}

unique_ptr<PrimitiveSceneProxy> StaticMeshComponent::CreateSceneProxy() {
    if (!ShouldCreateRenderState()) {
        return nullptr;
    }
    return make_unique<StaticMeshSceneProxy>(_mesh, _materials, GetWorldMatrix());
}

}  // namespace radray
