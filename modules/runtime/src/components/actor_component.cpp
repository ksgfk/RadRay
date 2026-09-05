#include <radray/runtime/components/actor_component.h>

#include <radray/runtime/game_framework/actor.h>

namespace radray {

Nullable<World*> ActorComponent::GetWorld() const noexcept {
    if (_owner) {
        return _owner.Get()->GetWorld();
    }
    return nullptr;
}

}  // namespace radray
