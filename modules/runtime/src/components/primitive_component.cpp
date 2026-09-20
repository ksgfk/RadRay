#include <radray/runtime/components/primitive_component.h>

namespace radray {

PrimitiveComponent::~PrimitiveComponent() noexcept = default;

void PrimitiveComponent::CreateRenderState(SceneWriter& writer) {
    _shapeId = writer.CreateShape();
    _sceneId = writer.GetSceneId();
    MarkRenderStateDirty();
    OnRenderStateCreated();
}

void PrimitiveComponent::DestroyRenderState(SceneWriter& writer) {
    OnRenderStateDestroyed();
    writer.RemoveShape(_shapeId);
    _shapeId = {};
    _sceneId = {};
}

void PrimitiveComponent::CollectRenderUpdates(SceneWriter& writer, RenderDirtyFlags dirty) {
    writer.Queue(writer.GetShape(_shapeId));
    CollectPrimitiveUpdates(writer, dirty);
}

void PrimitiveComponent::OnTransformChanged() { MarkRenderTransformDirty(); }

}  // namespace radray
