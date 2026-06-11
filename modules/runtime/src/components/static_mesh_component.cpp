#include <radray/runtime/components/static_mesh_component.h>

#include <radray/runtime/renderer/static_mesh_scene_proxy.h>

namespace radray {

void StaticMeshComponent::SetStaticMesh(AssetRef<StaticMesh> mesh) noexcept {
    _mesh = std::move(mesh);
    // TODO: 如果已注册,重建 SceneProxy
}

unique_ptr<PrimitiveSceneProxy> StaticMeshComponent::CreateSceneProxy() {
    if (!_mesh || !_material) {
        return nullptr;
    }
    return make_unique<StaticMeshSceneProxy>(_mesh, _material);
}

}  // namespace radray
