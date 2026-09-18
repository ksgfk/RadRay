#pragma once

#include <cstdint>
#include <limits>
#include <optional>
#include <span>

#include <radray/types.h>
#include <radray/runtime/static_mesh.h>

namespace radray {

struct PrimitiveId {
    uint32_t Index{std::numeric_limits<uint32_t>::max()};
    uint32_t Generation{0};

    bool IsValid() const noexcept { return Index != std::numeric_limits<uint32_t>::max() && Generation != 0; }
    bool operator==(const PrimitiveId&) const noexcept = default;
};

/// Owned CPU metadata and an immutable GPU view borrowed under the current flight's asset pins.
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

/// Sealed on GT together with asset pins before the runner publishes the flight to RT.
struct SceneUpdateBatch {
    vector<PrimitiveId> RemovePrimitives;
    vector<PrimitiveId> CreatePrimitives;
    vector<StaticMeshStateUpdate> MeshStates;
    vector<PrimitiveTransformUpdate> Transforms;

    void Clear() noexcept;
    bool Empty() const noexcept { return RemovePrimitives.empty() && CreatePrimitives.empty() && MeshStates.empty() && Transforms.empty(); }
};

/// RT borrow until the next Apply or Scene destruction. RenderMesh requires this flight's active asset pins.
struct StaticMeshSceneView {
    const StaticMeshDescription& Mesh;
    const Eigen::Matrix4f& LocalToWorld;
    const Eigen::Vector3f& WorldBoundsMin;
    const Eigen::Vector3f& WorldBoundsMax;
    bool ReverseCulling;
};

class StaticMeshProxy;

/// A single persistent CPU scene. RT owns Apply and reads; GT may inspect only after RT stops.
class Scene {
public:
    Scene() noexcept;
    ~Scene() noexcept;
    Scene(const Scene&) = delete;
    Scene& operator=(const Scene&) = delete;
    Scene(Scene&&) noexcept;
    Scene& operator=(Scene&&) noexcept;

    /// Must run once per published batch, in order, after all preceding CPU scene readers finish.
    void Apply(const SceneUpdateBatch& batch) noexcept;
    bool ContainsPrimitive(PrimitiveId id) const noexcept;
    std::optional<StaticMeshSceneView> GetStaticMesh(PrimitiveId id) const noexcept;
    /// Dense registered mesh identities, including meshes whose geometry is not ready.
    std::span<const PrimitiveId> GetStaticMeshes() const noexcept { return _staticMeshes; }

private:
    struct PrimitiveSlot {
        uint32_t Generation{0};
        bool Alive{false};
        unique_ptr<StaticMeshProxy> Mesh;
        size_t MeshIndex{std::numeric_limits<size_t>::max()};
    };
    vector<PrimitiveSlot> _primitives;
    vector<PrimitiveId> _staticMeshes;
};

}  // namespace radray
