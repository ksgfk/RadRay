#include <radray/runtime/world_manager.h>

#include <algorithm>

#include <radray/logger.h>
#include <radray/scope_guard.h>
#include <radray/runtime/game_framework/world.h>

namespace radray {

WorldManager::WorldManager(Nullable<Application*> app, Nullable<RenderSystem*> renderer)
    : _app(app), _renderSystem(renderer) {}

WorldManager::~WorldManager() noexcept {
    Clear();
}

WorldId WorldManager::CreateWorld() {
    CheckCanModify();
    auto world = _app ? make_unique<World>(_app.Get()) : make_unique<World>();
    const auto handle = _worlds.Emplace(WorldRecord{std::move(world), false});
    const WorldId id{handle.Index, handle.Generation};
    _worldIds.push_back(id);
    return id;
}

Nullable<World*> WorldManager::GetWorld(WorldId id) noexcept {
    auto record = _worlds.TryGet({id.Index, id.Generation});
    return record && !record->PendingDestroy ? record->Value.get() : nullptr;
}

Nullable<const World*> WorldManager::GetWorld(WorldId id) const noexcept {
    auto record = _worlds.TryGet({id.Index, id.Generation});
    return record && !record->PendingDestroy ? record->Value.get() : nullptr;
}

void WorldManager::DestroyWorld(WorldId id) {
    CheckCanModify();
    auto record = _worlds.TryGet({id.Index, id.Generation});
    if (!record || record->PendingDestroy) RADRAY_ABORT("Invalid World destruction");
    record->Value->CheckCanModify();
    record->PendingDestroy = true;
}

SceneId WorldManager::AttachWorldToRendering(WorldId id) {
    CheckCanModify();
    auto world = GetWorld(id);
    if (!world || !_renderSystem) RADRAY_ABORT("Cannot attach World");
    return world->AttachToRendering(*_renderSystem);
}

void WorldManager::DetachWorldFromRendering(WorldId id) {
    CheckCanModify();
    auto world = GetWorld(id);
    if (!world) RADRAY_ABORT("Cannot detach World");
    world->DetachFromRendering();
}

void WorldManager::Tick(float deltaTime) {
    CheckIdle();
    _ticking = true;
    {
        auto guard = MakeScopeGuard([this]() noexcept { _ticking = false; });
        _tickWorldIds = _worldIds;
        for (const auto id : _tickWorldIds) {
            if (auto world = GetWorld(id)) world->Tick(deltaTime);
        }
    }
    CleanupDestroyedWorlds();
}

void WorldManager::CollectRenderUpdates() {
    CheckIdle();
    _collecting = true;
    auto guard = MakeScopeGuard([this]() noexcept { _collecting = false; });
    for (const auto id : _worldIds) {
        if (auto world = GetWorld(id)) world->CollectRenderUpdates();
    }
}

void WorldManager::Clear() {
    CheckIdle();
    for (const auto& record : _worlds.Values()) record.Value->CheckCanModify();
    _destroying = true;
    auto guard = MakeScopeGuard([this]() noexcept { _destroying = false; });
    for (auto& record : _worlds.Values()) record.PendingDestroy = true;
    // Run callbacks while the identity registry is stable, with every World hidden.
    for (auto& record : _worlds.Values()) record.Value.reset();
    _worlds.Clear();
    _worldIds.clear();
    _tickWorldIds.clear();
}

void WorldManager::CleanupDestroyedWorlds() {
    _destroying = true;
    auto guard = MakeScopeGuard([this]() noexcept { _destroying = false; });
    std::erase_if(_worldIds, [this](WorldId id) {
        auto& record = _worlds.Get({id.Index, id.Generation});
        if (!record.PendingDestroy) return false;
        // Destroy callbacks must not observe a registry mid swap-remove.
        auto world = std::move(record.Value);
        _worlds.Destroy({id.Index, id.Generation});
        world.reset();
        return true;
    });
}

void WorldManager::CheckCanModify() const noexcept {
    if (_collecting || _destroying) RADRAY_ABORT("Cannot modify WorldManager during collection or destruction");
}

void WorldManager::CheckIdle() const noexcept {
    CheckCanModify();
    if (_ticking) RADRAY_ABORT("Cannot reenter WorldManager during Tick");
}

}  // namespace radray
