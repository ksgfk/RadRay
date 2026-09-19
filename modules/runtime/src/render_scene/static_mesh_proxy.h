#pragma once

#include <radray/runtime/render_scene/render_scene.h>

namespace radray {

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
