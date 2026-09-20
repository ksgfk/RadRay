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

void Actor::DispatchTick(float deltaTime, uint64_t epoch) {
    if (!IsLive() || _firstTickEpoch > epoch) return;
    const size_t count = _ownedComponents.size();
    _world->BeginCallback();
    auto guard = MakeScopeGuard([this]() noexcept { _world->EndCallback(); });
    Tick(deltaTime);
    for (size_t i = 0; i < count && IsLive(); ++i) {
        auto* component = _ownedComponents[i].get();
        if (component->IsLive() && component->_firstTickEpoch <= epoch && component->IsRegistered()) {
            component->TickComponent(deltaTime);
        }
    }
}

void Actor::RegisterComponent(ActorComponent& component) {
    if (!IsLive() || !component.IsLive() || component._registration != ComponentRegistration::Unregistered) return;
    if (_world->GetCurrentTickEpoch() == std::numeric_limits<uint64_t>::max()) RADRAY_ABORT("Tick epoch exhausted");
    component._firstTickEpoch = _world->GetCurrentTickEpoch() + 1;
    component._registration = ComponentRegistration::Registering;
    _world->BeginCallback();
    auto guard = MakeScopeGuard([this]() noexcept { _world->EndCallback(); });
    if (auto scene = dynamic_cast<SceneComponent*>(&component)) _world->CreateComponentRenderState(*scene);
    component.OnRegister();
    component._registration = ComponentRegistration::Registered;
}

void Actor::UnregisterComponent(ActorComponent& component) {
    if (component._registration == ComponentRegistration::Unregistered) return;
    if (component._registration != ComponentRegistration::Registered) RADRAY_ABORT("Reentrant component unregistration");
    component._registration = ComponentRegistration::Unregistering;
    _world->BeginCallback();
    auto guard = MakeScopeGuard([this]() noexcept { _world->EndCallback(); });
    if (auto scene = dynamic_cast<SceneComponent*>(&component)) _world->DestroyComponentRenderState(*scene);
    component.OnUnregister();
    component._registration = ComponentRegistration::Unregistered;
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

void Actor::Teardown() {
    _lifecycle = ObjectLifecycle::Destroying;
    _rootComponent = nullptr;
    for (const auto& component : _ownedComponents) component->_lifecycle = ObjectLifecycle::Destroying;
    UnregisterAllComponents();
    if (_spawned) {
        _spawned = false;
        _world->BeginCallback();
        auto guard = MakeScopeGuard([this]() noexcept { _world->EndCallback(); });
        OnDestroyed();
    }
    for (const auto& component : _ownedComponents) component->_owner = nullptr;
    _ownedComponents.clear();
    _world = nullptr;
}

}  // namespace radray
