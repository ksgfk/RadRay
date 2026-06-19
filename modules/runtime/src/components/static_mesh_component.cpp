#include <radray/runtime/components/static_mesh_component.h>

#include <algorithm>

#include <radray/runtime/renderer/static_mesh_scene_proxy.h>

namespace radray {

RuntimeTypeId StaticMeshComponent::GetTypeId() const noexcept {
    return runtime_type_id_v<StaticMeshComponent>;
}

bool StaticMeshComponent::AreAssetsReady() const noexcept {
    const bool meshReady = _mesh.IsReady();
    const bool materialReady = _material.IsReady() || !_material.IsValid();
    const bool texturesReady = std::all_of(_materialTextures.begin(), _materialTextures.end(), [](const auto& texture) {
        return texture.Texture.IsReady();
    });
    return meshReady && materialReady && texturesReady;
}

void StaticMeshComponent::TickComponent(float deltaTime) {
    (void)deltaTime;
    // 资产尚未交付为 proxy 时,持续尝试;两者就绪后建一次 proxy。
    if (GetSceneProxy() != nullptr) {
        return;
    }
    if (AreAssetsReady()) {
        RecreateSceneProxy();
    }
}

unique_ptr<PrimitiveSceneProxy> StaticMeshComponent::CreateSceneProxy() {
    if (!_mesh || !_material) {
        return nullptr;
    }
    return make_unique<StaticMeshSceneProxy>(_mesh, _material, _materialParams, _materialTextures);
}

}  // namespace radray
