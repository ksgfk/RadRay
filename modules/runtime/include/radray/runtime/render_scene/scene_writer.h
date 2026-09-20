#pragma once

#include <radray/sparse_set.h>
#include <radray/runtime/render_scene/scene_update.h>
#include <radray/runtime/render_scene/render_asset_lifetime.h>

namespace radray {

class RenderSystem;

/// GT only. One producer owns a scene; borrows expire when destruction is requested.
class SceneWriter {
public:
    SceneWriter(SceneId id, uint32_t flightCount);
    ~SceneWriter() noexcept;
    SceneWriter(const SceneWriter&) = delete;
    SceneWriter& operator=(const SceneWriter&) = delete;

    SceneId GetSceneId() const noexcept { return _id; }
    ShapeId CreateShape();
    void RemoveShape(ShapeId id);
    void SetStaticMesh(ShapeId id, const StreamingAssetRef<StaticMesh>& mesh, const Eigen::Matrix4f& localToWorld);
    /// Requires a prior SetStaticMesh, including an explicitly empty mesh binding.
    void SetTransform(ShapeId id, const Eigen::Matrix4f& localToWorld);
    /// Reserves an identity; the light enters snapshots after its first SetLight.
    LightId CreateLight();
    void RemoveLight(LightId id);
    void SetLight(const LightData& light);

private:
    friend class RenderSystem;
    friend class PrimitiveComponent;

    struct ShapeState {
        ShapeId Id;
        bool Sent{false};
        bool HasMesh{false};
        size_t DirtyIndex{std::numeric_limits<size_t>::max()};
        std::optional<AssetId> Asset;
        std::optional<StaticMeshStateUpdate> Mesh;
        std::optional<Eigen::Matrix4f> Transform;
    };

    ShapeState& GetShape(ShapeId id);
    void Queue(ShapeState& state);
    void Unqueue(ShapeState& state);
    void Flush(SceneUpdateBatch& batch, uint32_t flightIndex);

    SceneId _id;
    SparseSet<ShapeState> _shapes;
    vector<ShapeId> _dirtyShapes;
    vector<ShapeId> _removedShapes;
    SparseSet<std::monostate> _lightIds;
    LightSceneData _lights;
    bool _lightsDirty{false};
    RenderAssetLifetime _assets;
    bool _closing{false};
    bool _claimed{false};
};

}  // namespace radray
