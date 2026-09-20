#include <radray/runtime/game_framework/world.h>

#include <algorithm>
#include <radray/logger.h>
#include <radray/scope_guard.h>
#include <radray/profiler.h>
#include <radray/runtime/game_framework/actor.h>
#include <radray/runtime/world_manager.h>
#include "world_render_bridge.h"

namespace radray {

World::World() = default;
World::World(Application* app) : _app(app) {}
World::~World() noexcept {
    if (!_actors.empty() || _renderBridge) RADRAY_ABORT("World requires explicit ShutdownWorld or WorldManager shutdown");
}

uint64_t World::GetCurrentTickEpoch() const noexcept { return _manager ? _manager->GetCurrentTickEpoch() : _tickEpoch; }

void World::CheckCanModify() const noexcept {
    if (std::this_thread::get_id() != _ownerThread) RADRAY_ABORT("World mutation requires its owning thread");
    if (_collecting || (_manager && _manager->_collecting)) RADRAY_ABORT("Cannot mutate World during render collection");
}

void World::CheckDriverIdle() const noexcept {
    CheckCanModify();
    if (_manager || _ticking || _committing || _callbackDepth) RADRAY_ABORT("Cannot reenter World driver");
}

void World::BeginCallback() noexcept {
    ++_callbackDepth;
    if (_manager) ++_manager->_callbackDepth;
}
void World::EndCallback() noexcept {
    --_callbackDepth;
    if (_manager) --_manager->_callbackDepth;
}

Actor* World::SpawnActor(unique_ptr<Actor> actor) {
    CheckCanModify();
    if (!IsLive() || _stopping || !actor || actor->_world) RADRAY_ABORT("Cannot spawn into unavailable World");
    if (GetCurrentTickEpoch() == std::numeric_limits<uint64_t>::max()) RADRAY_ABORT("Tick epoch exhausted");
    auto* raw = actor.get();
    for (const auto& component : raw->_ownedComponents) {
        if (auto scene = dynamic_cast<SceneComponent*>(component.get()); scene && !scene->CanJoinWorld(*raw, *this)) RADRAY_ABORT("Draft attachment crosses the target World");
    }
    const auto handle = _actorIds.Emplace(raw);
    raw->_id = {_id, handle.Index, handle.Generation};
    raw->_firstTickEpoch = GetCurrentTickEpoch() + 1;
    raw->_world = this;
    _actors.push_back(std::move(actor));
    for (const auto& component : raw->_ownedComponents) component->_id.Actor = raw->_id;
    BeginCallback();
    auto guard = MakeScopeGuard([this]() noexcept { EndCallback(); });
    raw->RegisterAllComponents();
    if (raw->IsLive()) {
        raw->_spawned = true;
        raw->OnSpawned();
    }
    if (raw->_lifecycle == ObjectLifecycle::Initializing) raw->_lifecycle = ObjectLifecycle::Live;
    return raw;
}

Nullable<Actor*> World::ResolveIncludingPending(ActorId id) const noexcept {
    if (id.World != _id) return nullptr;
    auto value = _actorIds.TryGet({id.Index, id.Generation});
    return value ? *value : nullptr;
}
Nullable<Actor*> World::FindLive(ActorId id) const noexcept {
    auto value = ResolveIncludingPending(id);
    return value && value->IsLive() ? value : nullptr;
}
Nullable<ActorComponent*> World::FindLive(ComponentId id) const noexcept {
    auto actor = FindLive(id.Actor);
    return actor ? actor->FindLive(id) : nullptr;
}

LifecycleRequestResult World::DestroyActor(Actor* actor) {
    CheckCanModify();
    if (actor->_world.Get() != this) return LifecycleRequestResult::Invalid;
    return DestroyActor(actor->GetId());
}
LifecycleRequestResult World::DestroyActor(ActorId id) {
    CheckCanModify();
    auto actor = ResolveIncludingPending(id);
    if (!actor || !IsLive()) return LifecycleRequestResult::Invalid;
    if (actor->_lifecycle == ObjectLifecycle::PendingDestroy) return LifecycleRequestResult::AlreadyPending;
    if (!actor->IsLive()) return LifecycleRequestResult::Invalid;
    actor->_lifecycle = ObjectLifecycle::PendingDestroy;
    _pending.Actors.push_back(id);
    return LifecycleRequestResult::Accepted;
}
void World::QueueComponentDestruction(ActorComponent& component) { _pending.Components.push_back(component.GetId()); }

void World::DispatchTick(float deltaTime, uint64_t epoch) {
    if (!IsLive() || !_tickEnabled || _firstTickEpoch > epoch || _stopping) return;
    _ticking = true;
    auto guard = MakeScopeGuard([this]() noexcept { _ticking = false; });
    const size_t count = _actors.size();
    for (size_t i = 0; i < count && IsLive(); ++i) {
        auto* actor = _actors[i].get();
        actor->DispatchTick(deltaTime, epoch);
    }
}
void World::Tick(float deltaTime) {
    CheckDriverIdle();
    if (_tickEpoch == std::numeric_limits<uint64_t>::max()) RADRAY_ABORT("Tick epoch exhausted");
    DispatchTick(deltaTime, ++_tickEpoch);
}

void World::FreezeLifecycle() {
    _committing = true;
    std::swap(_pending, _executing);
}

void World::PrepareLifecycle() {
    RADRAY_PROFILE_SCOPE_N("World::PrepareLifecycle");
    if (_lifecycle == ObjectLifecycle::Destroying) {
        for (const auto& actor : _actors) actor->_lifecycle = ObjectLifecycle::Destroying;
    } else {
        for (const auto id : _executing.Actors) {
            if (auto actor = ResolveIncludingPending(id)) actor->_lifecycle = ObjectLifecycle::Destroying;
        }
    }
    if (!_executing.Actors.empty() || _lifecycle == ObjectLifecycle::Destroying) {
        std::erase_if(_actors, [this](auto& owner) {
            if (owner->_lifecycle != ObjectLifecycle::Destroying) return false;
            const auto id = owner->GetId();
            if (id.Generation == std::numeric_limits<uint32_t>::max()) RADRAY_ABORT("Actor generation exhausted");
            _actorIds.Destroy({id.Index, id.Generation});
            _retiredActors.push_back(std::move(owner));
            return true;
        });
    }
    std::sort(_executing.Components.begin(), _executing.Components.end());
    for (size_t first = 0; first < _executing.Components.size();) {
        const auto actorId = _executing.Components[first].Actor;
        size_t end = first + 1;
        while (end < _executing.Components.size() && _executing.Components[end].Actor == actorId) ++end;
        auto actor = ResolveIncludingPending(actorId);
        if (actor) {
            for (size_t i = first; i < end; ++i) {
                if (auto component = actor->ResolveIncludingPending(_executing.Components[i])) component->_lifecycle = ObjectLifecycle::Destroying;
            }
            std::erase_if(actor->_ownedComponents, [this, actor](auto& owner) {
                if (owner->_lifecycle != ObjectLifecycle::Destroying) return false;
                const auto id = owner->GetId();
                if (id.Generation == std::numeric_limits<uint32_t>::max()) RADRAY_ABORT("Component generation exhausted");
                actor->_componentIds.Destroy({id.Index, id.Generation});
                if (actor->_rootComponent.Get() == owner.get()) actor->_rootComponent = nullptr;
                _retiredComponents.push_back(std::move(owner));
                return true;
            });
        }
        first = end;
    }
    for (const auto& actor : _retiredActors) {
        actor->_rootComponent = nullptr;
        for (const auto& component : actor->_ownedComponents) component->_lifecycle = ObjectLifecycle::Destroying;
    }
    for (const auto& actor : _retiredActors) {
        for (const auto& component : actor->_ownedComponents) {
            if (auto scene = dynamic_cast<SceneComponent*>(component.get())) scene->UnlinkHierarchy(&_detachedChildren);
        }
    }
    for (const auto& component : _retiredComponents) {
        if (auto scene = dynamic_cast<SceneComponent*>(component.get())) scene->UnlinkHierarchy(&_detachedChildren);
    }
}

void World::ExecuteLifecycle() {
    RADRAY_PROFILE_SCOPE_N("World::ExecuteLifecycle");
    for (auto* child : _detachedChildren) {
        if (child->IsLive()) child->NotifyTransformChanged();
    }
    _detachedChildren.clear();
    for (size_t i = _retiredComponents.size(); i > 0; --i) {
        auto& component = *_retiredComponents[i - 1];
        component._owner->UnregisterComponent(component);
        component._owner = nullptr;
    }
    for (const auto& actor : _retiredActors) {
        actor->Teardown();
    }
    for (const auto& request : _executing.Reparents) {
        auto child = FindLive(request.Child);
        auto parent = request.Parent ? FindLive(*request.Parent) : Nullable<ActorComponent*>{nullptr};
        if (!child || (request.Parent && !parent)) continue;
        auto* scene = dynamic_cast<SceneComponent*>(child.Get());
        auto* parentScene = dynamic_cast<SceneComponent*>(parent.Get());
        if (scene && !scene->ReparentNow(parentScene, request.Rule)) RADRAY_WARN_LOG("Rejected deferred attachment");
    }
    for (const auto& request : _executing.Roots) {
        auto actor = FindLive(request.Actor);
        auto component = request.Root ? FindLive(*request.Root) : Nullable<ActorComponent*>{nullptr};
        if (actor && (!request.Root || component)) actor->_rootComponent = dynamic_cast<SceneComponent*>(component.Get());
    }
    if (_executing.Connection && IsLive() && !_stopping) {
        const auto request = *_executing.Connection;
        if (request.Reconnect || request.Target != _renderer) {
            DisconnectNow();
            if (request.Target && IsLive()) {
                _renderer = request.Target;
                _renderBridge = make_unique<WorldRenderBridge>(*this, *request.Target.Get());
                _renderBridge->Initialize();
            }
        }
    }
    _retiredComponents.clear();
    _retiredActors.clear();
    _executing.Clear();
    _committing = false;
}

void World::FinalizeWorldGT() {
    CheckDriverIdle();
    FreezeLifecycle();
    PrepareLifecycle();
    ExecuteLifecycle();
}

void World::Teardown() {
    _stopping = true;
    _lifecycle = ObjectLifecycle::Destroying;
    FreezeLifecycle();
    PrepareLifecycle();
    ExecuteLifecycle();
    DisconnectNow();
    _pending.Clear();
}
void World::ShutdownWorld() {
    CheckDriverIdle();
    Teardown();
}

LifecycleRequestResult World::RequestRenderConnection(Nullable<RenderSystem*> renderer) {
    CheckCanModify();
    if (!IsLive() || _stopping) return LifecycleRequestResult::Invalid;
    if (_pending.Connection && _pending.Connection->Target == renderer) return LifecycleRequestResult::AlreadyPending;
    const bool reconnect = _pending.Connection && _pending.Connection->Reconnect;
    _pending.Connection = ConnectionRequest{renderer, reconnect};
    return LifecycleRequestResult::Accepted;
}
LifecycleRequestResult World::RequestReconnect() {
    CheckCanModify();
    if (!IsLive() || _stopping) return LifecycleRequestResult::Invalid;
    auto target = GetRequestedRenderConnection();
    if (!target) return LifecycleRequestResult::Invalid;
    _pending.Connection = ConnectionRequest{target, true};
    return LifecycleRequestResult::Accepted;
}
Nullable<RenderSystem*> World::GetRequestedRenderConnection() const noexcept {
    return _pending.Connection ? _pending.Connection->Target : _renderer;
}
RenderConnectionState World::GetRenderConnectionState() const noexcept {
    return _renderBridge ? _renderBridge->GetState() : RenderConnectionState::Disconnected;
}
void World::DisconnectNow() {
    if (_renderBridge) {
        _renderBridge->Disconnect();
        _renderBridge.reset();
    }
    _renderer = nullptr;
}
std::optional<SceneId> World::GetRenderSceneId() const noexcept {
    return _renderBridge ? std::optional{_renderBridge->GetSceneId()} : std::nullopt;
}

LifecycleRequestResult World::QueueReparent(SceneComponent& child, Nullable<SceneComponent*> parent, AttachmentRule rule) {
    CheckCanModify();
    if (!child.IsLive() || child.GetWorld().Get() != this || (parent && (!parent->IsLive() || parent->GetWorld().Get() != this))) return LifecycleRequestResult::Invalid;
    for (auto ancestor = parent; ancestor; ancestor = ancestor->GetAttachParent()) {
        if (ancestor.Get() == &child) return LifecycleRequestResult::Invalid;
    }
    _pending.Reparents.push_back({child.GetId(), parent ? std::optional{parent->GetId()} : std::nullopt, rule});
    return LifecycleRequestResult::Accepted;
}

void World::Collect() {
    _collecting = true;
    auto guard = MakeScopeGuard([this]() noexcept { _collecting = false; });
    if (_renderBridge) _renderBridge->Collect();
}
void World::CollectRenderUpdates() {
    CheckDriverIdle();
    Collect();
}
void World::CreateComponentRenderState(SceneComponent& component) {
    if (_renderBridge) _renderBridge->Create(component);
}
void World::DestroyComponentRenderState(SceneComponent& component) {
    if (_renderBridge) _renderBridge->Destroy(component);
}
void World::QueueRenderUpdate(SceneComponent& component, RenderDirtyFlag flag) {
    CheckCanModify();
    if (_renderBridge) _renderBridge->Queue(component, flag);
}

}  // namespace radray
