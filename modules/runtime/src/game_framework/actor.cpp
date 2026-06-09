#include <radray/runtime/game_framework/actor.h>

#include <algorithm>

#include <radray/runtime/components/actor_component.h>
#include <radray/runtime/components/scene_component.h>

namespace radray {

Actor::~Actor() noexcept {
    UnregisterAllComponents();
}

void Actor::RemoveComponent(ActorComponent* component) {
    if (component->_owner.Get() != this) {
        return;
    }
    if (component->IsRegistered()) {
        component->OnUnregister();
        component->_registered = false;
    }
    // If it's the root, clear it
    if (_rootComponent.Get() == component) {
        _rootComponent = nullptr;
    }
    // If it's a scene component, detach from hierarchy
    if (component->IsSceneComponent()) {
        static_cast<SceneComponent*>(component)->DetachFromParent();
    }
    component->_owner = nullptr;
    auto it = std::find_if(_ownedComponents.begin(), _ownedComponents.end(),
                           [component](const unique_ptr<ActorComponent>& ptr) {
                               return ptr.get() == component;
                           });
    if (it != _ownedComponents.end()) {
        _ownedComponents.erase(it);
    }
}

void Actor::SetRootComponent(Nullable<SceneComponent*> component) noexcept {
    if (component && component.Get()->_owner.Get() != this) {
        return;
    }
    _rootComponent = component;
}


void Actor::Tick(float deltaTime) {
    for (auto& comp : _ownedComponents) {
        comp->TickComponent(deltaTime);
    }
}

void Actor::AddComponentInternal(unique_ptr<ActorComponent> component) {
    component->_owner = this;
    ActorComponent* raw = component.get();
    _ownedComponents.push_back(std::move(component));
    // If already in world, register immediately
    if (_world) {
        raw->_registered = true;
        raw->OnRegister();
    }
}

void Actor::RegisterAllComponents() {
    for (auto& comp : _ownedComponents) {
        if (!comp->_registered) {
            comp->_registered = true;
            comp->OnRegister();
        }
    }
}

void Actor::UnregisterAllComponents() {
    // Unregister in reverse order
    for (auto it = _ownedComponents.rbegin(); it != _ownedComponents.rend(); ++it) {
        auto& comp = *it;
        if (comp->_registered) {
            comp->OnUnregister();
            comp->_registered = false;
        }
    }
}

}  // namespace radray
