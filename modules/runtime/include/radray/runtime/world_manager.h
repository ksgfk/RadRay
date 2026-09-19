#pragma once

#include <radray/nullable.h>
#include <radray/sparse_set.h>
#include <radray/types.h>
#include <radray/runtime/game_framework/world_id.h>
#include <radray/runtime/render_scene/scene_id.h>

namespace radray {

class Application;
class RenderSystem;
class World;

/// GT-only World ownership and scheduling. Borrowed services must outlive this manager.
class WorldManager {
public:
    explicit WorldManager(Nullable<Application*> app = nullptr, Nullable<RenderSystem*> renderer = nullptr);
    WorldManager(const WorldManager&) = delete;
    WorldManager(WorldManager&&) = delete;
    WorldManager& operator=(const WorldManager&) = delete;
    WorldManager& operator=(WorldManager&&) = delete;
    ~WorldManager() noexcept;

    WorldId CreateWorld();
    Nullable<World*> GetWorld(WorldId id) noexcept;
    Nullable<const World*> GetWorld(WorldId id) const noexcept;
    /// Immediately hides the World; Tick releases it after all scheduled callbacks return.
    void DestroyWorld(WorldId id);
    SceneId AttachWorldToRendering(WorldId id);
    void DetachWorldFromRendering(WorldId id);

    /// Worlds created during Tick start ticking on the next call.
    void Tick(float deltaTime);
    /// Collects connected worlds, including paused worlds. Call after Tick and before sealing.
    void CollectRenderUpdates();
    /// Immediately destroys all worlds and invalidates their IDs; forbidden inside callbacks.
    /// Scene retirement remains the RenderSystem's responsibility.
    void Clear();

private:
    void CleanupDestroyedWorlds();
    void CheckCanModify() const noexcept;
    void CheckIdle() const noexcept;

    struct WorldRecord {
        unique_ptr<World> Value;
        bool PendingDestroy{false};
    };
    Nullable<Application*> _app;
    Nullable<RenderSystem*> _renderSystem;
    SparseSet<WorldRecord> _worlds;
    vector<WorldId> _worldIds;
    vector<WorldId> _tickWorldIds;
    bool _ticking{false};
    bool _collecting{false};
    bool _destroying{false};
};

}  // namespace radray
