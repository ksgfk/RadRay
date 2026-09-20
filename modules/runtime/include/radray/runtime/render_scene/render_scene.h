#pragma once

#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <condition_variable>
#include <mutex>

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
    RenderScene(RenderScene&&) = delete;
    RenderScene& operator=(RenderScene&&) = delete;

    class ReadLease {
    public:
        ReadLease(const ReadLease&) = delete;
        ReadLease& operator=(const ReadLease&) = delete;
        ReadLease(ReadLease&& other) noexcept;
        ReadLease& operator=(ReadLease&& other) noexcept;
        ~ReadLease() noexcept;

    private:
        friend class RenderScene;
        explicit ReadLease(const RenderScene* scene) noexcept;
        void Release() noexcept;
        Nullable<const RenderScene*> _scene;
    };
    /// Acquire on RT before dispatch; this lease may be moved to and released by the worker.
    ReadLease AcquireRead() const;

    /// Must run once per published batch, in order, after all preceding CPU scene readers finish.
    void Apply(const SceneUpdateBatch& batch) noexcept;
    bool ContainsShape(ShapeId id) const noexcept;
    std::optional<StaticMeshSceneView> GetStaticMesh(ShapeId id) const noexcept;
    bool ContainsLight(LightId id) const noexcept { return static_cast<bool>(_lights.GetLight(id)); }
    /// Common parameters; type-specific borrows are available through GetLights().
    Nullable<const LightCommonData*> GetLight(LightId id) const noexcept { return _lights.GetLight(id); }
    const LightSceneData& GetLights() const noexcept { return _lights; }
    /// Dense registered mesh identities, including meshes whose geometry is not ready.
    std::span<const ShapeId> GetStaticMeshes() const noexcept { return _staticMeshes; }

private:
    struct ShapeSlot {
        // Live identity, or the minimum generation accepted by the next creation.
        uint32_t Generation{0};
        bool Alive{false};
        unique_ptr<StaticMeshProxy> Mesh;
        size_t MeshIndex{std::numeric_limits<size_t>::max()};
    };
    vector<ShapeSlot> _shapes;
    vector<ShapeId> _staticMeshes;
    LightSceneData _lights;
    mutable std::mutex _readers;
    mutable std::condition_variable _readersDone;
    mutable size_t _activeReaders{0};
};

}  // namespace radray
