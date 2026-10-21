#pragma once

#include <radray/sparse_set.h>
#include <radray/runtime/render_framework/scene_update.h>
#include <radray/runtime/render_framework/render_asset_lifetime.h>

namespace radray {

class SceneManager;

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
    void SetStaticMeshes(std::span<const ShapeId> ids, const StreamingAssetRef<StaticMesh>& mesh, std::span<const Eigen::Matrix4f> transforms);
    /// Requires a prior SetStaticMesh, including an explicitly empty mesh binding.
    void SetMaterials(ShapeId id, std::span<const StreamingAssetRef<Material>> materials);
    void SetMaterials(std::span<const ShapeId> ids, std::span<const StreamingAssetRef<Material>> materials);
    bool CreateShapes(std::span<ShapeId> result);
    void RemoveShapes(std::span<const ShapeId> ids);
    void SetTransforms(std::span<const ShapeTransformUpdate> values);
    void SetTransform(ShapeId id, const Eigen::Matrix4f& localToWorld);
    bool SetInstanceParameters(ShapeId id, const InstanceParameterLayout& layout, std::span<const byte> values);
    bool SetInstanceParameters(std::span<const ShapeId> ids, const InstanceParameterLayout& layout, std::span<const byte> values);
    void SetDrawState(ShapeId id, ShapeDrawState state);
    void SetDrawStates(std::span<const ShapeDrawUpdate> values);
    uint64_t CreateBatchDomain();
    /// Reserves an identity; the light enters snapshots after its first SetLight.
    LightId CreateLight();
    void RemoveLight(LightId id);
    void SetLight(LightId id, const LightData& light);

private:
    friend class SceneManager;
    friend class SceneCapture;
    friend class ShapeCapture;
    friend class WorldRenderBridge;

    static constexpr size_t kNotQueued = std::numeric_limits<size_t>::max();
    static constexpr uint32_t kNoLightRow = std::numeric_limits<uint32_t>::max();

    struct MaterialUse {
        MaterialBindings Bindings;
        size_t Hash, Count{0};
    };
    struct ShapeState {
        /// An engaged value owns one use in _assets, including an empty AssetId.
        std::optional<AssetId> BoundAsset;
        bool Sent{false};
        bool HasMesh{false};
        Nullable<MaterialUse*> BoundMaterials{nullptr};
        size_t MaterialUpdateIndex{kNotQueued};
        struct ParameterIndex {
            InstanceSchemaId Schema;
            size_t Index;
        };
        vector<ParameterIndex> ParameterUpdateIndices;
        size_t DrawUpdateIndex{kNotQueued};
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
    void RemoveShapeState(ShapeId id, ShapeState& state);
    ShapeEdit& GetEdit(ShapeId id);
    void EnableEditing();
    void ReleaseMaterials(ShapeState& state);
    struct ParameterPayload {
        vector<uint32_t> Free;
    };
    void ReclaimParameter(const ShapeParameterUpdate& update);
    void WriteDrawState(const ShapeDrawUpdate& value);
    void BeginCapture();
    void QueueCreate(ShapeId id, ShapeState& state);
    void CancelCreate(ShapeEdit& edit);
    template <class T>
    void CancelUpdate(vector<T>& updates, ShapeEdit& edit);
    void WriteStaticMesh(ShapeId id, ShapeState& state, const StreamingAssetRef<StaticMesh>& mesh, const AffineTransform& localToWorld, TransformId transform = {});
    void WriteMeshState(ShapeId id, ShapeState& state, StaticMeshDescription mesh, const AffineTransform& localToWorld, TransformId transform);
    void GatherStaticMeshes(std::span<const ShapeId> ids, const StreamingAssetRef<StaticMesh>& mesh, std::span<const Eigen::Matrix4f> transforms);
    void GatherTransforms(std::span<const ShapeTransformUpdate> values);
    void WriteTransform(ShapeId id, ShapeState& state, const AffineTransform& localToWorld);
    void Flush(SceneUpdateBatch& batch, uint32_t flightIndex, bool requireExitConfirmation);
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
    unordered_map<size_t, vector<unique_ptr<MaterialUse>>> _materialBindings;
    unordered_map<string, InstanceParameterLayout> _parameterSchemas;
    unordered_map<uint32_t, ParameterPayload> _parameterPayloads;
    bool _closing{false};
    bool _claimed{false};
    bool _editing{false};
    bool _captured{false};
    uint64_t _nextBatchDomain{1};
};

}  // namespace radray
