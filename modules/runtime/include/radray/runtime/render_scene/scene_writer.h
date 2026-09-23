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
    void SetLight(LightId id, const LightData& light);

private:
    friend class RenderSystem;
    friend class SceneCapture;
    friend class ShapeCapture;
    friend class WorldRenderBridge;

    static constexpr size_t kNotQueued = std::numeric_limits<size_t>::max();
    static constexpr uint32_t kNoLightRow = std::numeric_limits<uint32_t>::max();

    struct ShapeState {
        /// An engaged value owns one use in _assets, including an empty AssetId.
        std::optional<AssetId> BoundAsset;
        bool Sent{false};
        bool HasMesh{false};
    };

    struct ShapeEdit {
        size_t CreateIndex{kNotQueued};
        size_t UpdateIndex{kNotQueued};
        bool PendingMesh{false};
    };

    struct LightSlot {
        uint32_t Row{kNoLightRow};
        uint32_t Type{0};
    };

    struct TransformState {
        size_t CreateIndex{kNotQueued}, LocalIndex{kNotQueued}, ParentIndex{kNotQueued};
        uint64_t EditEpoch{0};
        TransformId Parent;
        bool Sent{false};
    };
    TransformId CreateTransform(TransformId parent, const LocalTransform& local);
    void RemoveTransform(TransformId id);
    void SetLocalTransforms(std::span<const LocalTransformUpdate> updates);
    void GatherLocalTransforms(std::span<const LocalTransformUpdate> values, std::span<const uint32_t> indices);
    void WriteLocalTransform(const LocalTransformUpdate& update);
    void ReserveLocalTransforms(size_t count);
    void SetTransformParent(TransformId id, TransformId parent);
    void BeginTransformEdit(TransformState& state) noexcept {
        if (state.EditEpoch == _transformEditEpoch) return;
        state.EditEpoch = _transformEditEpoch;
        state.LocalIndex = state.ParentIndex = kNotQueued;
    }

    ShapeState& GetShape(ShapeId id);
    ShapeEdit& GetEdit(ShapeId id);
    void EnableEditing();
    void BeginCapture();
    void QueueCreate(ShapeId id, ShapeState& state);
    void CancelCreate(ShapeEdit& edit);
    template <class T>
    void CancelUpdate(vector<T>& updates, ShapeEdit& edit);
    void WriteStaticMesh(ShapeId id, ShapeState& state, const StreamingAssetRef<StaticMesh>& mesh, const AffineTransform& localToWorld, TransformId transform = {});
    void WriteTransform(ShapeId id, ShapeState& state, const AffineTransform& localToWorld);
    void Flush(SceneUpdateBatch& batch, uint32_t flightIndex);
    void RemoveLightData(LightSlot& slot);

    SceneId _id;
    SparseSet<ShapeState> _shapes;
    SparseSet<TransformState> _transforms;
    uint64_t _transformEditEpoch{1};
    /// Only independent editing or repeated captures before Seal need update locators.
    vector<ShapeEdit> _edits;
    SceneUpdateBatch _pending;
    SparseSet<LightSlot> _lightIds;
    LightSceneData _lights;
    RenderAssetLifetime _assets;
    bool _closing{false};
    bool _claimed{false};
    bool _editing{false};
    bool _captured{false};
};

}  // namespace radray
