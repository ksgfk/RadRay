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

void ActorComponent::SetTickEnabled(bool enabled) noexcept {
    CheckCanModify();
    if (_tickEnabled == enabled) return;
    _tickEnabled = enabled;
    if (auto* owner = _owner.Get(); owner && _registration == ComponentRegistration::Registered) {
        owner->NoteTickingComponent(enabled ? 1 : -1);
    }
}

}  // namespace radray
