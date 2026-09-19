#pragma once

#include <radray/runtime/render_scene/scene_id.h>
#include <radray/runtime/static_mesh.h>

namespace radray {

/// Owned CPU metadata and an immutable GPU view borrowed from GT-owned scene assets.
struct StaticMeshDescription {
    AssetId MeshAssetId;
    Nullable<const GpuMesh*> RenderMesh{nullptr};
    vector<StaticMeshSection> Sections;
    Eigen::Vector3f LocalBoundsMin{Eigen::Vector3f::Zero()};
    Eigen::Vector3f LocalBoundsMax{Eigen::Vector3f::Zero()};
};

struct StaticMeshStateUpdate {
    PrimitiveId Id;
    StaticMeshDescription Mesh{};
    Eigen::Matrix4f LocalToWorld{Eigen::Matrix4f::Identity()};
};

struct PrimitiveTransformUpdate {
    PrimitiveId Id;
    Eigen::Matrix4f LocalToWorld{Eigen::Matrix4f::Identity()};
};

/// Sealed on GT together with asset retirements before the runner publishes the flight to RT.
struct SceneUpdateBatch {
    vector<PrimitiveId> RemovePrimitives;
    vector<PrimitiveId> CreatePrimitives;
    vector<StaticMeshStateUpdate> MeshStates;
    vector<PrimitiveTransformUpdate> Transforms;

    void Clear() noexcept;
    bool Empty() const noexcept { return RemovePrimitives.empty() && CreatePrimitives.empty() && MeshStates.empty() && Transforms.empty(); }
};

struct SceneFrameUpdate {
    SceneId Id;
    bool Create{false};
    bool Destroy{false};
    SceneUpdateBatch Updates;
};

}  // namespace radray
