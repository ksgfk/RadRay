#include <radray/runtime/components/static_mesh_component.h>

#include <utility>

namespace radray {

StaticMeshComponent::~StaticMeshComponent() noexcept = default;

void StaticMeshComponent::SetStaticMesh(StreamingAssetRef<StaticMesh> mesh) {
    if (_mesh == mesh) {
        return;
    }
    _mesh = std::move(mesh);
}

}  // namespace radray
