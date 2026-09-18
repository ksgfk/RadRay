#include <radray/runtime/components/primitive_component.h>

#include <radray/runtime/game_framework/world.h>

namespace radray {

PrimitiveComponent::~PrimitiveComponent() noexcept = default;

void PrimitiveComponent::CreateRenderState(World& world) {
    _primitiveId = world.AllocatePrimitiveId();
    MarkRenderStateDirty();
}

void PrimitiveComponent::DestroyRenderState(World& world) {
    if (_renderStateSent) world._removedPrimitives.push_back(_primitiveId);
    world.ReleasePrimitiveId(_primitiveId);
    _primitiveId = {};
    _renderStateSent = false;
}

void PrimitiveComponent::CollectRenderUpdates(SceneUpdateBatch& batch, RenderDirtyFlags dirty) {
    if (!_renderStateSent) batch.CreatePrimitives.push_back(_primitiveId);
    CollectPrimitiveUpdates(batch, dirty);
    _renderStateSent = true;
}

void PrimitiveComponent::OnTransformChanged() {
    MarkRenderTransformDirty();
}

}  // namespace radray
