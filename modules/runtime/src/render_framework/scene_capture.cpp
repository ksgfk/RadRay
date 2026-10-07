#include <radray/runtime/render_framework/scene_capture.h>

namespace radray {

ShapeCapture::ShapeCapture(SceneWriter& writer, ShapeId id) : _writer(writer), _id(id), _state(writer.GetShape(id)) {
    _writer.QueueCreate(id, _state);
}
void ShapeCapture::CheckUnwritten() {
    if (_written) RADRAY_ABORT("A shape capture must emit only its final value");
    _written = true;
}
void ShapeCapture::SetStaticMesh(const StreamingAssetRef<StaticMesh>& mesh, const AffineTransform& transform) {
    CheckUnwritten();
    _writer.WriteStaticMesh(_id, _state, mesh, transform);
}
void ShapeCapture::SetTransform(const AffineTransform& transform) {
    CheckUnwritten();
    _writer.WriteTransform(_id, _state, transform);
}
void ShapeCapture::SetStaticMesh(const StreamingAssetRef<StaticMesh>& mesh, TransformId transform) {
    CheckUnwritten();
    if (!transform.IsValid()) RADRAY_ABORT("A bound mesh requires a scene transform");
    _writer.WriteStaticMesh(_id, _state, mesh, AffineTransform{}, transform);
}

void ShapeCapture::SetMaterials(std::span<const StreamingAssetRef<Material>> materials) {
    if (_materialsWritten) RADRAY_ABORT("A capture must emit only final materials");
    _materialsWritten = true;
    _writer.SetMaterials(_id, materials);
}

SceneCapture::SceneCapture(SceneWriter& writer) : _writer(writer) { writer.BeginCapture(); }
void SceneCapture::SetLight(LightId id, const LightData& light) { _writer.SetLight(id, light); }

}  // namespace radray
