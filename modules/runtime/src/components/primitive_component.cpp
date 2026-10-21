#include <radray/runtime/components/primitive_component.h>

namespace radray {

PrimitiveComponent::~PrimitiveComponent() noexcept = default;

void PrimitiveComponent::CreateRenderState(SceneWriter& writer) {
    _shapeId = writer.CreateShape();
    MarkRenderStateDirty();
    OnRenderStateCreated();
}

void PrimitiveComponent::DestroyRenderState(SceneWriter& writer) {
    OnRenderStateDestroyed();
    writer.RemoveShape(_shapeId);
    _shapeId = {};
}

void PrimitiveComponent::CollectRenderUpdates(SceneCapture& capture, RenderDirtyFlags dirty) {
    auto shape = capture.CaptureShape(_shapeId);
    CollectPrimitiveUpdates(shape, dirty);
}

}  // namespace radray
