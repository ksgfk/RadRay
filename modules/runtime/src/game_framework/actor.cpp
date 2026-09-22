#include <radray/runtime/game_framework/actor.h>

#include <algorithm>

#include <radray/logger.h>
#include <radray/scope_guard.h>
#include <radray/runtime/components/scene_component.h>
#include <radray/runtime/game_framework/world.h>

namespace radray {

Actor::~Actor() noexcept {
    if (_world) RADRAY_ABORT("Registered Actor requires explicit teardown");
}

bool Actor::IsLive() const noexcept {
    return (_lifecycle == ObjectLifecycle::Live || _lifecycle == ObjectLifecycle::Initializing) && (!_world || _world->IsLive());
}

ActorComponent* Actor::AddComponent(unique_ptr<ActorComponent> component) {
    return AddComponent(std::move(component), nullptr, AttachmentRule::KeepLocal);
}

ActorComponent* Actor::AddComponent(unique_ptr<ActorComponent> component, Nullable<SceneComponent*> parent, AttachmentRule rule) {
    if (_world) _world->CheckCanModify();
    if (!IsLive() || !component || component->_owner) RADRAY_ABORT("Cannot add component to unavailable Actor");
    if (_world && _world->_stopping) RADRAY_ABORT("Cannot create components while stopping");
    auto scene = dynamic_cast<SceneComponent*>(component.get());
    if (_world && scene && !scene->CanJoinWorld(*this, *_world.Get())) RADRAY_ABORT("Draft attachment crosses the target World");
    if (parent && (!scene || !parent->IsLive() || parent->GetWorld() != _world)) RADRAY_ABORT("Invalid initial attachment");
    if (parent) {
        Eigen::Vector3f location, scale;
        Eigen::Quaternionf rotation;
        if (!scene->ComputeAttachmentTransform(parent, rule, location, rotation, scale)) RADRAY_ABORT("Invalid initial attachment transform");
    }
    auto* raw = component.get();
    const auto handle = _componentIds.Emplace(raw);
    raw->_id = {_id, handle.Index, handle.Generation};
    raw->_owner = this;
    raw->_world = _world;
    _ownedComponents.push_back(std::move(component));
    if (parent && !scene->ReparentNow(parent, rule)) RADRAY_ABORT("Initial attachment cannot preserve the requested transform");
    if (_world) RegisterComponent(*raw);
    if (raw->_lifecycle == ObjectLifecycle::Initializing) raw->_lifecycle = ObjectLifecycle::Live;
    return raw;
}

Nullable<ActorComponent*> Actor::ResolveIncludingPending(ComponentId id) const noexcept {
    if (id.Actor != _id) return nullptr;
    auto object = _componentIds.TryGet({id.Index, id.Generation});
    return object ? *object : nullptr;
}

Nullable<ActorComponent*> Actor::FindLive(ComponentId id) const noexcept {
    auto component = ResolveIncludingPending(id);
    return component && component->IsLive() ? component : nullptr;
}

LifecycleRequestResult Actor::RemoveComponent(ActorComponent* component) {
    if (_world) _world->CheckCanModify();
    if (component->_owner.Get() != this || _lifecycle == ObjectLifecycle::Destroying) return LifecycleRequestResult::Invalid;
    if (component->_lifecycle == ObjectLifecycle::PendingDestroy) return LifecycleRequestResult::AlreadyPending;
    if (!component->IsLive()) return LifecycleRequestResult::Invalid;
    component->_lifecycle = ObjectLifecycle::PendingDestroy;
    if (_world) {
        _world->QueueComponentDestruction(*component);
    } else {
        // Unregistered drafts have no user lifecycle to dispatch.
        if (_rootComponent.Get() == component) _rootComponent = nullptr;
        if (auto scene = dynamic_cast<SceneComponent*>(component)) scene->UnlinkHierarchy(nullptr);
        const auto id = component->_id;
        if (id.Generation == std::numeric_limits<uint32_t>::max()) RADRAY_ABORT("Component generation exhausted");
        _componentIds.Destroy({id.Index, id.Generation});
        component->_owner = nullptr;
        component->_world = nullptr;
        std::erase_if(_ownedComponents, [component](const auto& value) { return value.get() == component; });
    }
    return LifecycleRequestResult::Accepted;
}

void Actor::SetRootComponent(Nullable<SceneComponent*> component) noexcept {
    if (_world) RADRAY_ABORT("Registered root replacement requires RequestSetRootComponent");
    if (component && component->GetOwner().Get() != this) RADRAY_ABORT("Invalid draft root");
    _rootComponent = component;
}

LifecycleRequestResult Actor::RequestSetRootComponent(Nullable<SceneComponent*> component) {
    if (!_world) return LifecycleRequestResult::Invalid;
    _world->CheckCanModify();
    if (!IsLive() || (component && (component->GetOwner().Get() != this || !component->IsLive()))) return LifecycleRequestResult::Invalid;
    _world->_pending.Roots.push_back({_id, component ? std::optional{component->GetId()} : std::nullopt});
    return LifecycleRequestResult::Accepted;
}

void Actor::SetTickEnabled(bool enabled) noexcept {
    if (_world) _world->CheckCanModify();
    if (_tickEnabled == enabled) return;
    _tickEnabled = enabled;
    if (_world) _world->RefreshTicking(*this);
}

void Actor::NoteTickingComponent(int32_t delta) noexcept {
    if (delta > 0) {
        _tickingComponents += static_cast<uint32_t>(delta);
    } else {
        const auto sub = static_cast<uint32_t>(-delta);
        if (_tickingComponents < sub) RADRAY_ABORT("Ticking component count underflow");
        _tickingComponents -= sub;
    }
    if (_world) _world->RefreshTicking(*this);
}

void Actor::DispatchTick(float deltaTime, uint64_t epoch) {
    if (!IsLive() || _firstTickEpoch > epoch) return;
    if (!_tickEnabled && _tickingComponents == 0) return;
    const size_t count = _ownedComponents.size();
    _world->BeginCallback();
    auto guard = MakeScopeGuard([this]() noexcept { _world->EndCallback(); });
    if (_tickEnabled) Tick(deltaTime);
    if (_tickingComponents == 0) return;
    for (size_t i = 0; i < count && IsLive(); ++i) {
        auto* component = _ownedComponents[i].get();
        if (component->_tickEnabled && component->IsLive() && component->_firstTickEpoch <= epoch && component->IsRegistered()) {
            component->TickComponent(deltaTime);
        }
    }
}

void Actor::RegisterComponent(ActorComponent& component) {
    if (!IsLive() || !component.IsLive() || component._registration != ComponentRegistration::Unregistered) return;
    if (_world->GetCurrentTickEpoch() == std::numeric_limits<uint64_t>::max()) RADRAY_ABORT("Tick epoch exhausted");
    component._world = _world;
    component._firstTickEpoch = _world->GetCurrentTickEpoch() + 1;
    component._registration = ComponentRegistration::Registering;
    _world->BeginCallback();
    auto guard = MakeScopeGuard([this]() noexcept { _world->EndCallback(); });
    if (auto scene = dynamic_cast<SceneComponent*>(&component)) _world->CreateComponentRenderState(*scene);
    component.OnRegister();
    component._registration = ComponentRegistration::Registered;
    if (component._tickEnabled) NoteTickingComponent(1);
}

void Actor::UnregisterComponent(ActorComponent& component) {
    if (component._registration == ComponentRegistration::Unregistered) return;
    if (component._registration != ComponentRegistration::Registered) RADRAY_ABORT("Reentrant component unregistration");
    if (component._tickEnabled) NoteTickingComponent(-1);
    component._registration = ComponentRegistration::Unregistering;
    _world->BeginCallback();
    auto guard = MakeScopeGuard([this]() noexcept { _world->EndCallback(); });
    if (auto scene = dynamic_cast<SceneComponent*>(&component)) _world->DestroyComponentRenderState(*scene);
    component.OnUnregister();
    component._registration = ComponentRegistration::Unregistered;
    component._world = nullptr;
}

void Actor::RegisterAllComponents() {
    const size_t count = _ownedComponents.size();
    for (size_t i = 0; i < count && IsLive(); ++i) {
        auto* component = _ownedComponents[i].get();
        component->_id.Actor = _id;
        RegisterComponent(*component);
    }
}

void Actor::UnregisterAllComponents() {
    const size_t count = _ownedComponents.size();
    for (size_t i = count; i > 0; --i) UnregisterComponent(*_ownedComponents[i - 1]);
}

void Actor::PrepareComponentDestruction(std::span<const ComponentId> ids, vector<unique_ptr<ActorComponent>>& retired) {
    for (const auto id : ids) {
        if (auto component = ResolveIncludingPending(id)) component->_lifecycle = ObjectLifecycle::Destroying;
    }
    std::erase_if(_ownedComponents, [this, &retired](auto& owner) {
        if (owner->_lifecycle != ObjectLifecycle::Destroying) return false;
        const auto id = owner->GetId();
        if (id.Generation == std::numeric_limits<uint32_t>::max()) RADRAY_ABORT("Component generation exhausted");
        _componentIds.Destroy({id.Index, id.Generation});
        if (_rootComponent.Get() == owner.get()) _rootComponent = nullptr;
        retired.push_back(std::move(owner));
        return true;
    });
}

void Actor::PrepareComponentTeardown() noexcept {
    _rootComponent = nullptr;
    for (const auto& component : _ownedComponents) component->_lifecycle = ObjectLifecycle::Destroying;
}

void Actor::Teardown() {
    _lifecycle = ObjectLifecycle::Destroying;
    PrepareComponentTeardown();
    UnregisterAllComponents();
    if (_spawned) {
        _spawned = false;
        _world->BeginCallback();
        auto guard = MakeScopeGuard([this]() noexcept { _world->EndCallback(); });
        OnDestroyed();
    }
    for (const auto& component : _ownedComponents) {
        component->_owner = nullptr;
        component->_world = nullptr;
    }
    _ownedComponents.clear();
    _world = nullptr;
}

}  // namespace radray
