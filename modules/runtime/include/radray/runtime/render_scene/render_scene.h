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
#include <radray/runtime/render_scene/static_mesh_proxy.h>

namespace radray {

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
    static constexpr uint32_t kNoMesh = std::numeric_limits<uint32_t>::max();

    /// Identity and routing only; geometry lives in the dense arrays below. Randomly accessed by
    /// every transform update, so keep it small.
    struct ShapeSlot {
        // Live identity, or the minimum generation accepted by the next creation.
        uint32_t Generation{0};
        // Position in _staticMeshes / _staticMeshProxies, or kNoMesh when no geometry is registered.
        uint32_t MeshIndex{kNoMesh};
        bool Alive{false};
    };
    vector<ShapeSlot> _shapes;
    /// Parallel dense arrays: _staticMeshes[i] owns _staticMeshProxies[i]. Removal swaps in the last entry.
    vector<ShapeId> _staticMeshes;
    vector<StaticMeshProxy> _staticMeshProxies;
    LightSceneData _lights;
    mutable std::mutex _readers;
    mutable std::condition_variable _readersDone;
    mutable size_t _activeReaders{0};
};

}  // namespace radray
