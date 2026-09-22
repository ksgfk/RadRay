#include <radray/runtime/render_scene/scene_capture.h>

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

SceneCapture::SceneCapture(SceneWriter& writer) : _writer(writer) { writer.BeginCapture(); }
void SceneCapture::SetLight(LightId id, const LightData& light) { _writer.SetLight(id, light); }

}  // namespace radray
