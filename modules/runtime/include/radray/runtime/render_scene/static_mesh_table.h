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

/// Dense mesh columns reference the shared matrix pool through TransformRows. Borrows expire at Apply.
struct StaticMeshSceneColumns {
    std::span<const ShapeId> Ids;
    std::span<const StaticMeshDescription> Bindings;
    std::span<const Eigen::Matrix4f> Transforms;
    std::span<const uint32_t> TransformRows;
    std::span<const StaticMeshBounds> Bounds;

    size_t Size() const noexcept { return Ids.size(); }
    StaticMeshSceneView Get(size_t row) const noexcept;
};

class StaticMeshTable {
public:
    StaticMeshSceneColumns GetColumns(std::span<const Eigen::Matrix4f> transforms = {}) const noexcept { return {_ids, _bindings, transforms, _transformRows, _bounds}; }
    void Add(const StaticMeshStateUpdate& update, uint32_t transformRow);
    void Remove(size_t row) noexcept;
    void Replace(size_t row, const StaticMeshStateUpdate& update, uint32_t transformRow);
    void UpdateBounds(size_t row, const Eigen::Matrix4f& transform) noexcept;

private:
    vector<ShapeId> _ids;
    vector<StaticMeshDescription> _bindings;
    vector<uint32_t> _transformRows;
    vector<StaticMeshBounds> _bounds;
};

}  // namespace radray
