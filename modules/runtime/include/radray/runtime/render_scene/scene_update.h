#pragma once

#include <radray/runtime/render_scene/scene_id.h>
#include <radray/runtime/render_scene/light_scene_data.h>
#include <radray/runtime/static_mesh.h>

namespace radray {

/// Immutable metadata and GPU views borrowed from GT-owned scene assets.
struct StaticMeshDescription {
    AssetId MeshAssetId;
    Nullable<const GpuMesh*> RenderMesh{nullptr};
    std::span<const StaticMeshSection> Sections;
    Eigen::Vector3f LocalBoundsMin{Eigen::Vector3f::Zero()};
    Eigen::Vector3f LocalBoundsMax{Eigen::Vector3f::Zero()};
};

struct StaticMeshStateUpdate {
    ShapeId Id;
    StaticMeshDescription Mesh{};
    Eigen::Matrix4f LocalToWorld{Eigen::Matrix4f::Identity()};
};

struct ShapeTransformUpdate {
    ShapeId Id;
    Eigen::Matrix4f LocalToWorld{Eigen::Matrix4f::Identity()};
};

/// Sealed on GT together with asset retirements before the runner publishes the flight to RT.
struct SceneUpdateBatch {
    vector<ShapeId> RemoveShapes;
    vector<ShapeId> CreateShapes;
    vector<StaticMeshStateUpdate> MeshStates;
    vector<ShapeTransformUpdate> Transforms;
    /// False preserves the RT light set; true replaces it, including with an empty list.
    bool LightsChanged{false};
    LightSceneData Lights;

    void Clear() noexcept;
    bool Empty() const noexcept { return RemoveShapes.empty() && CreateShapes.empty() && MeshStates.empty() && Transforms.empty() && !LightsChanged && Lights.Empty(); }
};

struct SceneFrameUpdate {
    SceneId Id;
    bool Create{false};
    bool Destroy{false};
    SceneUpdateBatch Updates;
};

}  // namespace radray
