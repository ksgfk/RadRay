#pragma once

#include <radray/runtime/render_framework/scene_writer.h>

namespace radray {

/// One final capture of an owned shape. Emit at most one state or transform; emitting neither keeps a bare shape.
class ShapeCapture {
public:
    ShapeCapture(const ShapeCapture&) = delete;
    ShapeCapture& operator=(const ShapeCapture&) = delete;
    void SetStaticMesh(const StreamingAssetRef<StaticMesh>& mesh, const AffineTransform& transform);
    void SetStaticMesh(const StreamingAssetRef<StaticMesh>& mesh, TransformId transform);
    void SetTransform(const AffineTransform& transform);
    void SetMaterials(std::span<const StreamingAssetRef<Material>> materials);
    bool SetInstanceParameters(const InstanceParameterLayout& layout, std::span<const byte> values);

private:
    friend class SceneCapture;
    ShapeCapture(SceneWriter& writer, ShapeId id);
    void CheckUnwritten();
    SceneWriter& _writer;
    ShapeId _id;
    SceneWriter::ShapeState& _state;
    bool _written{false};
    bool _materialsWritten{false};
};

/// World collection output. Each source captures each of its owned identities once per collection.
/// Business components cannot be mutated here; a source may create/remove its own batch shape identities.
class SceneCapture {
public:
    ShapeCapture CaptureShape(ShapeId id) { return ShapeCapture{_writer, id}; }
    void SetLight(LightId id, const LightData& light);
    bool CreateShapes(std::span<ShapeId> ids);
    void RemoveShapes(std::span<const ShapeId> ids);
    void SetStaticMeshes(std::span<const ShapeId> ids, const StreamingAssetRef<StaticMesh>& mesh, std::span<const Eigen::Matrix4f> transforms);
    void SetTransforms(std::span<const ShapeTransformUpdate> values);
    void SetMaterials(std::span<const ShapeId> ids, std::span<const StreamingAssetRef<Material>> materials);
    bool SetInstanceParameters(std::span<const ShapeId> ids, const InstanceParameterLayout& layout, std::span<const byte> values);
    void SetDrawStates(std::span<const ShapeDrawUpdate> values);

private:
    friend class WorldRenderBridge;
    explicit SceneCapture(SceneWriter& writer);
    SceneWriter& _writer;
};

}  // namespace radray
