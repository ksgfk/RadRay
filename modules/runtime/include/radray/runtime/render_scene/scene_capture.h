#pragma once

#include <radray/runtime/render_scene/scene_writer.h>

namespace radray {

/// One final capture of an owned shape. Emit at most one state or transform; emitting neither keeps a bare shape.
class ShapeCapture {
public:
    ShapeCapture(const ShapeCapture&) = delete;
    ShapeCapture& operator=(const ShapeCapture&) = delete;
    void SetStaticMesh(const StreamingAssetRef<StaticMesh>& mesh, const AffineTransform& transform);
    void SetStaticMesh(const StreamingAssetRef<StaticMesh>& mesh, TransformId transform);
    void SetTransform(const AffineTransform& transform);

private:
    friend class SceneCapture;
    ShapeCapture(SceneWriter& writer, ShapeId id);
    void CheckUnwritten();
    SceneWriter& _writer;
    ShapeId _id;
    SceneWriter::ShapeState& _state;
    bool _written{false};
};

/// World collection output. Each source captures each of its owned identities once per collection.
/// Components and scene identities cannot be mutated while this output is borrowed.
class SceneCapture {
public:
    ShapeCapture CaptureShape(ShapeId id) { return ShapeCapture{_writer, id}; }
    void SetLight(LightId id, const LightData& light);

private:
    friend class WorldRenderBridge;
    explicit SceneCapture(SceneWriter& writer);
    SceneWriter& _writer;
};

}  // namespace radray
