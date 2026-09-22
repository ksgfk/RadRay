#include <radray/runtime/world_manager.h>

#include <algorithm>
#include <radray/logger.h>
#include <radray/scope_guard.h>
#include <radray/runtime/game_framework/world.h>

namespace radray {

WorldManager::WorldManager(Nullable<Application*> app, Nullable<RenderSystem*> renderer)
    : _app(app), _renderSystem(renderer) {}
WorldManager::~WorldManager() noexcept {
    if (!_worlds.Empty()) RADRAY_ABORT("WorldManager requires explicit Shutdown");
}

WorldId WorldManager::CreateWorld() {
    CheckCanModify();
    if (_stopping) RADRAY_ABORT("Cannot create World while stopping");
    if (_tickEpoch == std::numeric_limits<uint64_t>::max()) RADRAY_ABORT("Tick epoch exhausted");
    auto world = _app ? make_unique<World>(_app.Get()) : make_unique<World>();
    auto* object = world.get();
    const auto handle = _worlds.Emplace(std::move(world));
    const WorldId id{handle.Index, handle.Generation};
    object->_manager = this;
    object->_id = id;
    object->_firstTickEpoch = _tickEpoch + 1;
    _worldIds.push_back(id);
    return id;
}
Nullable<World*> WorldManager::GetWorld(WorldId id) noexcept {
    auto record = _worlds.TryGet({id.Index, id.Generation});
    return record && (*record)->IsLive() ? record->get() : nullptr;
}
Nullable<const World*> WorldManager::GetWorld(WorldId id) const noexcept {
    auto record = _worlds.TryGet({id.Index, id.Generation});
    return record && (*record)->IsLive() ? record->get() : nullptr;
}
LifecycleRequestResult WorldManager::DestroyWorld(WorldId id) {
    CheckCanModify();
    auto record = _worlds.TryGet({id.Index, id.Generation});
    if (!record) return LifecycleRequestResult::Invalid;
    if ((*record)->_lifecycle == ObjectLifecycle::PendingDestroy) return LifecycleRequestResult::AlreadyPending;
    if (!(*record)->IsLive()) return LifecycleRequestResult::Invalid;
    (*record)->_lifecycle = ObjectLifecycle::PendingDestroy;
    _pendingDestroy.push_back(id);
    return LifecycleRequestResult::Accepted;
}
LifecycleRequestResult WorldManager::RequestRenderConnection(WorldId id, bool connected) {
    CheckCanModify();
    auto world = GetWorld(id);
    if (!world || (connected && !_renderSystem)) return LifecycleRequestResult::Invalid;
    return world->RequestRenderConnection(connected ? _renderSystem : Nullable<RenderSystem*>{nullptr});
}
LifecycleRequestResult WorldManager::RequestReconnect(WorldId id) {
    CheckCanModify();
    auto world = GetWorld(id);
    return world ? world->RequestReconnect() : LifecycleRequestResult::Invalid;
}
void WorldManager::Tick(float deltaTime) {
    CheckIdle();
    if (_stopping) return;
    if (_tickEpoch == std::numeric_limits<uint64_t>::max()) RADRAY_ABORT("Tick epoch exhausted");
    ++_tickEpoch;
    _ticking = true;
    auto guard = MakeScopeGuard([this]() noexcept { _ticking = false; });
    // Callbacks may append; deletion waits for S1. Copy each ID before calling out.
    const size_t count = _worldIds.size();
    for (size_t i = 0; i < count; ++i) {
        const auto id = _worldIds[i];
        if (auto world = GetWorld(id)) world->DispatchTick(deltaTime, _tickEpoch);
    }
}
void WorldManager::FinalizeWorldsGT() {
    CheckIdle();
    _committing = true;
    auto guard = MakeScopeGuard([this]() noexcept { _committing = false; });
    for (const auto id : _pendingDestroy) {
        auto record = _worlds.TryGet({id.Index, id.Generation});
        if (!record) continue;
        if (id.Generation == std::numeric_limits<uint32_t>::max()) RADRAY_ABORT("World generation exhausted");
        (*record)->_lifecycle = ObjectLifecycle::Destroying;
        (*record)->_stopping = true;
        _retiredWorlds.push_back(std::move(*record));
        _worlds.Destroy({id.Index, id.Generation});
    }
    if (!_pendingDestroy.empty()) {
        std::erase_if(_worldIds, [this](WorldId id) { return !_worlds.IsAlive({id.Index, id.Generation}); });
    }
    // No callbacks have run yet; requests made by the following hooks stay pending.
    _pendingDestroy.clear();
    for (const auto id : _worldIds) {
        auto* world = _worlds.Get({id.Index, id.Generation}).get();
        if (!world->_pending.Empty()) _commitWorlds.push_back(world);
    }
    for (const auto& world : _retiredWorlds) _commitWorlds.push_back(world.get());
    for (auto* world : _commitWorlds) world->FreezeLifecycle();
    for (auto* world : _commitWorlds) world->PrepareLifecycle();
    for (auto* world : _commitWorlds) world->ExecuteLifecycle();
    for (const auto& world : _retiredWorlds) world->DisconnectNow();
    _retiredWorlds.clear();
    _commitWorlds.clear();
    const auto count = _worldIds.size();
    for (size_t i = 0; i < count; ++i) {
        if (auto world = GetWorld(_worldIds[i])) world->DispatchTransforms(true);
    }
}
void WorldManager::CollectRenderUpdates() {
    CheckIdle();
    _collecting = true;
    auto guard = MakeScopeGuard([this]() noexcept { _collecting = false; });
    for (const auto id : _worldIds) {
        if (auto world = GetWorld(id)) world->Collect();
    }
}
void WorldManager::Clear() {
    CheckIdle();
    const bool stopping = _stopping;
    BeginStopping();
    for (const auto id : _worldIds) DestroyWorld(id);
    FinalizeWorldsGT();
    _worlds.Clear();
    _stopping = stopping;
}
void WorldManager::BeginStopping() {
    CheckIdle();
    _stopping = true;
    for (const auto& world : _worlds.Values()) world->_stopping = true;
}
void WorldManager::Shutdown() {
    BeginStopping();
    Clear();
}
void WorldManager::CheckCanModify() const noexcept {
    if (std::this_thread::get_id() != _ownerThread) RADRAY_ABORT("WorldManager requires its owning thread");
    if (_collecting) RADRAY_ABORT("Cannot modify WorldManager during collection");
}
void WorldManager::CheckIdle() const noexcept {
    CheckCanModify();
    if (_ticking || _committing || _callbackDepth) RADRAY_ABORT("Cannot reenter WorldManager driver");
}

}  // namespace radray
