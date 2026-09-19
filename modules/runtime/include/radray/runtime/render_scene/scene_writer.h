#pragma once

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
    PrimitiveId CreatePrimitive();
    void RemovePrimitive(PrimitiveId id);
    void SetStaticMesh(PrimitiveId id, const StreamingAssetRef<StaticMesh>& mesh, const Eigen::Matrix4f& localToWorld);
    /// Requires a prior SetStaticMesh, including an explicitly empty mesh binding.
    void SetTransform(PrimitiveId id, const Eigen::Matrix4f& localToWorld);

private:
    friend class RenderSystem;

    struct PrimitiveState {
        PrimitiveId Id;
        bool Sent{false};
        bool HasMesh{false};
        size_t DirtyIndex{std::numeric_limits<size_t>::max()};
        std::optional<AssetId> Asset;
        std::optional<StaticMeshStateUpdate> Mesh;
        std::optional<Eigen::Matrix4f> Transform;
    };

    PrimitiveState& GetPrimitive(PrimitiveId id);
    void Queue(PrimitiveState& state);
    void Unqueue(PrimitiveState& state);
    void Flush(SceneUpdateBatch& batch, uint32_t flightIndex);

    SceneId _id;
    SparseSet<PrimitiveState> _primitives;
    vector<PrimitiveId> _dirty;
    vector<PrimitiveId> _removed;
    RenderAssetLifetime _assets;
    bool _closing{false};
    bool _claimed{false};
};

}  // namespace radray
