#pragma once

#include <thread>

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
    LifecycleRequestResult DestroyWorld(WorldId id);
    LifecycleRequestResult RequestRenderConnection(WorldId id, bool connected);
    LifecycleRequestResult RequestReconnect(WorldId id);
    uint64_t GetCurrentTickEpoch() const noexcept { return _tickEpoch; }

    /// Worlds created during Tick start ticking on the next call.
    void Tick(float deltaTime);
    /// S1: freeze all request types, finish CPU lifetimes, then collect final source values.
    void FinalizeWorldsGT();
    /// Collects connected worlds, including paused worlds. Call after Tick and before sealing.
    void CollectRenderUpdates();
    /// Immediately destroys all worlds and invalidates their IDs; forbidden inside callbacks.
    /// Scene retirement remains the RenderSystem's responsibility.
    void Clear();
    void BeginStopping();
    void Shutdown();

private:
    friend class World;
    void CheckCanModify() const noexcept;
    void CheckIdle() const noexcept;

    Nullable<Application*> _app;
    Nullable<RenderSystem*> _renderSystem;
    SparseSet<unique_ptr<World>> _worlds;
    vector<WorldId> _worldIds;
    vector<WorldId> _pendingDestroy;
    vector<World*> _commitWorlds;
    vector<unique_ptr<World>> _retiredWorlds;
    std::thread::id _ownerThread{std::this_thread::get_id()};
    uint64_t _tickEpoch{0};
    uint32_t _callbackDepth{0};
    bool _ticking{false};
    bool _collecting{false};
    bool _committing{false};
    bool _stopping{false};
};

}  // namespace radray
