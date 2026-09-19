#include <radray/runtime/components/primitive_component.h>

namespace radray {

PrimitiveComponent::~PrimitiveComponent() noexcept = default;

void PrimitiveComponent::CreateRenderState(SceneWriter& writer) {
    _primitiveId = writer.CreatePrimitive();
    _sceneId = writer.GetSceneId();
    MarkRenderStateDirty();
    OnRenderStateCreated();
}

void PrimitiveComponent::DestroyRenderState(SceneWriter& writer) {
    OnRenderStateDestroyed();
    writer.RemovePrimitive(_primitiveId);
    _primitiveId = {};
    _sceneId = {};
}

void PrimitiveComponent::CollectRenderUpdates(SceneWriter& writer, RenderDirtyFlags dirty) {
    CollectPrimitiveUpdates(writer, dirty);
}

void PrimitiveComponent::OnTransformChanged() { MarkRenderTransformDirty(); }

}  // namespace radray
