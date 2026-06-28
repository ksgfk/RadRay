#pragma once

#include <span>

#include <radray/basic_math.h>
#include <radray/render/gpu_resource.h>
#include <radray/runtime/asset_manager.h>
#include <radray/runtime/render_framework/primitive_scene_proxy.h>
#include <radray/runtime/static_mesh.h>
#include <radray/types.h>

namespace radray {

class StaticMesh;
class StaticMeshComponent;

/// Render-side proxy for StaticMeshComponent.
/// Corresponds to UE5's FStaticMeshSceneProxy.
class StaticMeshSceneProxy final : public PrimitiveSceneProxy {
public:
    struct Section {
        uint32_t PrimitiveIndex{0};
        uint32_t FirstIndex{0};
        uint32_t IndexCount{0};
        uint32_t MinVertexIndex{0};
        uint32_t MaxVertexIndex{0};
    };

    StaticMeshSceneProxy(const StaticMeshComponent& component, StreamingAssetRef<StaticMesh> mesh);
    ~StaticMeshSceneProxy() noexcept override;

    StaticMesh* GetStaticMesh() const noexcept { return _mesh.Get(); }
    const render::RenderMesh* GetRenderMesh() const noexcept;
    const StreamingAssetRef<StaticMesh>& GetStaticMeshRef() const noexcept { return _mesh; }
    const Eigen::Matrix4f& GetLocalToWorld() const noexcept { return _localToWorld; }
    const Eigen::Vector3f& GetLocalBoundsMin() const noexcept { return _localBoundsMin; }
    const Eigen::Vector3f& GetLocalBoundsMax() const noexcept { return _localBoundsMax; }
    std::span<const Section> GetSections() const noexcept { return _sections; }

private:
    StreamingAssetRef<StaticMesh> _mesh;
    Eigen::Matrix4f _localToWorld;
    Eigen::Vector3f _localBoundsMin;
    Eigen::Vector3f _localBoundsMax;
    vector<Section> _sections;
};

}  // namespace radray
