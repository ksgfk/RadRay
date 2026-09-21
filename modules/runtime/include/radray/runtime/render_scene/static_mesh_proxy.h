#pragma once

#include <radray/runtime/render_scene/scene_update.h>

namespace radray {

/// RT borrow until the next Apply or RenderScene destruction; asset reads require the RenderScene's GT lifetime owner.
struct StaticMeshSceneView {
    const StaticMeshDescription& Mesh;
    const Eigen::Matrix4f& LocalToWorld;
    const Eigen::Vector3f& WorldBoundsMin;
    const Eigen::Vector3f& WorldBoundsMax;
    bool ReverseCulling;
};

/// Stored by value in RenderScene's dense static mesh array; Apply may relocate records,
/// so views and interior pointers only stay valid until the next Apply.
class StaticMeshProxy {
public:
    explicit StaticMeshProxy(const StaticMeshStateUpdate& update);
    void Replace(const StaticMeshStateUpdate& update);
    void SetTransform(const Eigen::Matrix4f& localToWorld) noexcept;
    StaticMeshSceneView GetView() const noexcept;

private:
    StaticMeshDescription _mesh;
    Eigen::Matrix4f _localToWorld;
    Eigen::Vector3f _worldBoundsMin;
    Eigen::Vector3f _worldBoundsMax;
    bool _reverseCulling{false};
};

}  // namespace radray
