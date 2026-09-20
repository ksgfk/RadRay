#include <radray/runtime/components/actor_component.h>

#include <radray/runtime/game_framework/actor.h>
#include <radray/runtime/game_framework/world.h>

namespace radray {

bool ActorComponent::IsLive() const noexcept {
    return (_lifecycle == ObjectLifecycle::Live || _lifecycle == ObjectLifecycle::Initializing) && (!_owner || _owner->IsLive());
}

void ActorComponent::CheckCanModify() const noexcept {
    if (auto world = GetWorld()) world->CheckCanModify();
}

Nullable<World*> ActorComponent::GetWorld() const noexcept {
    if (_owner) {
        return _owner.Get()->GetWorld();
    }
    return nullptr;
}

}  // namespace radray
