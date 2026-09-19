#pragma once

#include <cstdint>
#include <limits>
#include <optional>
#include <span>

#include <radray/types.h>
#include <radray/runtime/static_mesh.h>
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

class StaticMeshProxy;

/// A single persistent CPU scene. RT owns Apply and reads; GT may inspect only after RT stops.
class RenderScene {
public:
    RenderScene() noexcept;
    ~RenderScene() noexcept;
    RenderScene(const RenderScene&) = delete;
    RenderScene& operator=(const RenderScene&) = delete;
    RenderScene(RenderScene&&) noexcept;
    RenderScene& operator=(RenderScene&&) noexcept;

    /// Must run once per published batch, in order, after all preceding CPU scene readers finish.
    void Apply(const SceneUpdateBatch& batch) noexcept;
    bool ContainsPrimitive(PrimitiveId id) const noexcept;
    std::optional<StaticMeshSceneView> GetStaticMesh(PrimitiveId id) const noexcept;
    /// Dense registered mesh identities, including meshes whose geometry is not ready.
    std::span<const PrimitiveId> GetStaticMeshes() const noexcept { return _staticMeshes; }

private:
    struct PrimitiveSlot {
        // Live identity, or the minimum generation accepted by the next creation.
        uint32_t Generation{0};
        bool Alive{false};
        unique_ptr<StaticMeshProxy> Mesh;
        size_t MeshIndex{std::numeric_limits<size_t>::max()};
    };
    vector<PrimitiveSlot> _primitives;
    vector<PrimitiveId> _staticMeshes;
};

}  // namespace radray
