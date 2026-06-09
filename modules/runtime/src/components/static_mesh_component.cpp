#include <radray/runtime/components/static_mesh_component.h>

#include <radray/vertex_data.h>
#include <radray/runtime/renderer/primitive_scene_proxy.h>

namespace radray {

void StaticMeshComponent::SetMeshResource(shared_ptr<MeshResource> mesh) noexcept {
    _meshResource = std::move(mesh);
    // TODO: 如果已注册，重建 SceneProxy
}

unique_ptr<PrimitiveSceneProxy> StaticMeshComponent::CreateSceneProxy() {
    if (!_meshResource) {
        return nullptr;
    }
    // 当前返回基础 Proxy，后续 StaticMeshSceneProxy 子类会持有 GPU 资源
    return make_unique<PrimitiveSceneProxy>();
}

}  // namespace radray
