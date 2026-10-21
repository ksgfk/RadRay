#include <radray/runtime/components/render_component.h>

#include <radray/runtime/game_framework/world.h>

namespace radray {

RenderComponent::~RenderComponent() noexcept {
    if (_renderIndex != std::numeric_limits<uint32_t>::max()) RADRAY_ABORT("RenderComponent requires explicit render disconnection");
}

SceneId RenderComponent::GetRenderSceneId() const noexcept {
    const auto world = GetWorld();
    return world ? world->GetRenderSceneId().value_or(SceneId{}) : SceneId{};
}

void RenderComponent::MarkRenderDirty(RenderDirtyFlag flag) {
    if (_renderIndex == std::numeric_limits<uint32_t>::max()) return;
    const auto registration = GetRegistrationState();
    const auto life = GetLifecycle();
    if ((registration != ComponentRegistration::Registering && registration != ComponentRegistration::Registered) ||
        (life != ObjectLifecycle::Live && life != ObjectLifecycle::Initializing)) return;
    GetWorld()->EnqueueRenderDirty(*this, flag);
}
void RenderComponent::MarkRenderStateDirty() {
    CheckCanModify();
    MarkRenderDirty(RenderDirtyFlag::State);
}
void RenderComponent::MarkRenderTransformDirty() {
    CheckCanModify();
    MarkRenderDirty(RenderDirtyFlag::Transform);
}
void RenderComponent::MarkRenderDynamicDataDirty() {
    CheckCanModify();
    MarkRenderDirty(RenderDirtyFlag::DynamicData);
}
void RenderComponent::CollectRenderTransform(SceneCapture& capture) {
    if (UsesSceneTransform()) return;
    if (_renderIndex == std::numeric_limits<uint32_t>::max()) return;
    const auto dirty = _renderDirty | RenderDirtyFlag::Transform;
    _renderDirty = {};
    CollectRenderUpdates(capture, dirty);
}

}  // namespace radray
