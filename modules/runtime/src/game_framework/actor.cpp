#include <radray/runtime/game_framework/actor.h>

#include <algorithm>

#include <radray/runtime/components/actor_component.h>
#include <radray/runtime/components/scene_component.h>
#include <radray/runtime/game_framework/world.h>

namespace radray {

Actor::~Actor() noexcept {
    UnregisterAllComponents();
}

ActorComponent* Actor::AddComponent(unique_ptr<ActorComponent> component) {
    if (_world) _world->CheckCanModify();
    if (component == nullptr) {
        return nullptr;
    }

    component->_owner = this;
    ActorComponent* raw = component.get();
    _ownedComponents.push_back(std::move(component));
    // 若已在 World 中,立即注册
    if (_world) {
        RegisterComponent(*raw);
    }
    return raw;
}

void Actor::RemoveComponent(ActorComponent* component) {
    if (_world) _world->CheckCanModify();
    if (component->_owner.Get() != this) {
        return;
    }
    UnregisterComponent(*component);
    // 若是根组件,清空
    if (_rootComponent.Get() == component) {
        _rootComponent = nullptr;
    }
    // 若是 SceneComponent,从层级中摘除
    if (auto* sceneComponent = dynamic_cast<SceneComponent*>(component); sceneComponent != nullptr) {
        sceneComponent->DetachFromParent();
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
    if (_world) _world->CheckCanModify();
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

void Actor::RegisterComponent(ActorComponent& component) {
    if (component._registered) return;
    component._registered = true;
    if (Nullable<SceneComponent*> scene = dynamic_cast<SceneComponent*>(&component); scene) {
        _world->CreateComponentRenderState(*scene.Get());
    }
    component.OnRegister();
}

void Actor::UnregisterComponent(ActorComponent& component) {
    if (!component._registered) return;
    component._registered = false;
    if (Nullable<SceneComponent*> scene = dynamic_cast<SceneComponent*>(&component); scene) {
        _world->DestroyComponentRenderState(*scene.Get());
    }
    component.OnUnregister();
}

void Actor::RegisterAllComponents() {
    for (auto& comp : _ownedComponents) RegisterComponent(*comp);
}

void Actor::UnregisterAllComponents() {
    // 按逆序反注册
    for (auto it = _ownedComponents.rbegin(); it != _ownedComponents.rend(); ++it) {
        UnregisterComponent(**it);
    }
}

}  // namespace radray
