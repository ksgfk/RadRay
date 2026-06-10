#pragma once

#include <radray/runtime/components/primitive_component.h>

namespace radray {

class MeshResource;

/// 持有 MeshResource 的静态网格组件。
/// 对应 UE5 的 UStaticMeshComponent。
class StaticMeshComponent : public PrimitiveComponent {
public:
    StaticMeshComponent() noexcept = default;
    ~StaticMeshComponent() noexcept override = default;

    void SetMeshResource(shared_ptr<MeshResource> mesh) noexcept;
    const shared_ptr<MeshResource>& GetMeshResource() const noexcept { return _meshResource; }

    unique_ptr<PrimitiveSceneProxy> CreateSceneProxy() override;

private:
    shared_ptr<MeshResource> _meshResource;
};

}  // namespace radray
