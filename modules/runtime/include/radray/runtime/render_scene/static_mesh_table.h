#pragma once

#include <radray/runtime/render_scene/scene_update.h>

namespace radray {

struct StaticMeshBounds {
    Eigen::Vector3f Min;
    Eigen::Vector3f Max;
    bool ReverseCulling{false};
};

/// RT borrows expire at the next Apply; parallel workers must hold a RenderScene read lease.
struct StaticMeshSceneView {
    const StaticMeshDescription& Mesh;
    const Eigen::Matrix4f& LocalToWorld;
    const Eigen::Vector3f& WorldBoundsMin;
    const Eigen::Vector3f& WorldBoundsMax;
    bool ReverseCulling;
};

/// Parallel dense columns. Row numbers are transient, ShapeId is the persistent identity.
struct StaticMeshSceneColumns {
    std::span<const ShapeId> Ids;
    std::span<const StaticMeshDescription> Bindings;
    std::span<const Eigen::Matrix4f> Transforms;
    std::span<const StaticMeshBounds> Bounds;

    size_t Size() const noexcept { return Ids.size(); }
    StaticMeshSceneView Get(size_t row) const noexcept;
};

class StaticMeshTable {
public:
    StaticMeshSceneColumns GetColumns() const noexcept { return {_ids, _bindings, _transforms, _bounds}; }
    void Add(const StaticMeshStateUpdate& update);
    void Remove(size_t row) noexcept;
    void Replace(size_t row, const StaticMeshStateUpdate& update);
    void SetTransform(size_t row, const AffineTransform& transform) noexcept;

private:
    vector<ShapeId> _ids;
    vector<StaticMeshDescription> _bindings;
    vector<Eigen::Matrix4f> _transforms;
    vector<StaticMeshBounds> _bounds;
};

}  // namespace radray
