#include <radray/runtime/components/actor_component.h>

#include <radray/runtime/game_framework/actor.h>

namespace radray {

RuntimeTypeId ActorComponent::GetTypeId() const noexcept {
    return runtime_type_id_v<ActorComponent>;
}

Nullable<World*> ActorComponent::GetWorld() const noexcept {
    if (_owner) {
        return _owner.Get()->GetWorld();
    }
    return nullptr;
}

}  // namespace radray
