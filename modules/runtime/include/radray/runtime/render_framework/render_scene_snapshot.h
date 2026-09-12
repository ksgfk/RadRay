#pragma once

#include <radray/inline_vector.h>
#include <radray/runtime/material.h>
#include <radray/runtime/render_framework/cpu_draw_record.h>
#include <radray/runtime/render_framework/light_scene_proxy.h>
#include <radray/runtime/render_framework/mesh_batch.h>
#include <radray/runtime/render_framework/primitive_scene_proxy.h>
#include <radray/runtime/render_framework/render_bounds.h>
#include <radray/runtime/render_framework/render_graph_runtime_options.h>

namespace radray {

class Scene;
struct SceneSnapshotPublication;

struct RenderPrimitiveData {
    SceneObjectId Id{};
    Eigen::Matrix4f LocalToWorld{Eigen::Matrix4f::Identity()};
    AxisAlignedBounds WorldBounds{};
    uint32_t LayerMask{0xffffffffu};
    bool DisableFrustumCulling{false};
    uint32_t FirstMeshBatch{0};
    uint32_t MeshBatchCount{0};
    uint64_t Generation{0}, MotionRevision{0};
    uint64_t RenderDataRevision{0}, TransformRevision{0};
};

struct RenderLightData {
    LightType Type{LightType::Directional};
    LightRenderParameters Parameters{};
    SphereBounds WorldBounds{};
    uint32_t LayerMask{0xffffffffu};
    bool CastShadow{true};
};

struct RenderSceneSnapshotStats {
    uint64_t InputPrimitives{0}, InputSections{0}, InputMaterials{0}, InputLights{0};
    uint64_t Primitives{0}, MeshBatches{0}, Materials{0}, Lights{0};
    uint64_t MissingGeometry{0}, EmptyDraw{0}, InvalidDrawRange{0}, MaterialUnavailable{0}, InvalidBounds{0};
    uint64_t RetainedAssets{0};
    uint64_t MaterialBytesCopied{0}, ScratchEntriesCreated{0};
    uint64_t PrimitiveStructuresRebuilt{0}, PrimitiveStructuresReused{0};
    uint64_t PrimitiveBoundsRebuilt{0}, PrimitiveBoundsReused{0};
    uint64_t MaterialsRebuilt{0}, MaterialsReused{0};
    uint64_t DrawRecordBuilds{0}, DrawRecordsReused{0}, DrawRecordStateSelects{0};
    uint64_t DrawRecordBytes{0}, CpuSceneBytes{0};
    uint64_t DrawRecordFullSyncs{0}, DrawRecordPrimitivesVisited{0}, DrawRecordCopies{0};
    uint64_t StaticRecipeCompiles{0}, BindingRecipeCompiles{0};
    uint64_t PublishedPages{0}, PublishedBytes{0}, PublishedMaterialBytes{0};
    uint64_t PublishedVariablePayloadBytes{0};
    /// Ownership transferred by canonical swap-removal; these bytes are not deep copies.
    uint64_t MaterialPayloadMoves{0}, MovedMaterialPayloadBytes{0};
    uint64_t LegacyMaterialsObserved{0}, LegacyBytesCompared{0}, PendingResourcesObserved{0};
    uint64_t SceneCommits{0}, SnapshotPublications{0};
    uint64_t AppliedTransforms{0}, EqualValueIgnored{0};
    // Peak vector capacities, measured in elements across reuse cycles.
    size_t PrimitiveHighWatermark{0}, BatchHighWatermark{0}, MaterialHighWatermark{0}, LightHighWatermark{0};
};

struct SnapshotChangedRange {
    uint32_t First{0}, Count{0};
};

/// Per-flight values. Geometry/texture payloads and programs must outlive flight retirement.
/// Primitive values are builder-owned; ResetForReuse discards their materialization state.
/// DrawRecords are the stable catalog for this published epoch; views consume compact indices.
struct RenderSceneSnapshot {
    RenderSceneSnapshot() = default;
    RenderSceneSnapshot(const RenderSceneSnapshot& other);
    RenderSceneSnapshot& operator=(const RenderSceneSnapshot& other);
    RenderSceneSnapshot(RenderSceneSnapshot&&) noexcept = default;
    RenderSceneSnapshot& operator=(RenderSceneSnapshot&&) noexcept = default;
    vector<RenderPrimitiveData> Primitives;
    vector<MeshBatch> MeshBatches;
    vector<MaterialRenderData> Materials;
    vector<RenderLightData> Lights;
    vector<DrawRecord> DrawRecords;
    vector<StaticBindingRecipe> BindingRecipes;
    vector<CpuGeometryBindingPlan> GeometryBindingPlans;
    vector<uint32_t> PrimitiveDrawBegin;
    vector<SnapshotChangedRange> ChangedPrimitiveRanges;
    uint64_t SceneEpoch{0};
    uint64_t PublicationId{0};
    uint64_t PublicationRevision{0}, ChangedFromPublicationRevision{0};
    bool Valid{false};
    bool HasPassPolicies{false};
    RenderSceneSnapshotStats Stats;

    void ResetForReuse() noexcept;

private:
    friend class SceneRenderState;
    shared_ptr<SceneSnapshotPublication> _publication;
};

/// Snapshot-owned table and nested payload storage; pending publication pages are measured by SceneRenderState.
RenderMemoryStats MeasureRenderSceneSnapshot(const RenderSceneSnapshot& snapshot) noexcept;

/// Game thread only, after acquiring a writable flight. Failure leaves an invalid, unpublished snapshot.
bool BuildRenderSceneSnapshot(const Scene& scene, RenderSceneSnapshot& out, vector<StreamingAssetRefAny>& retainedAssets,
                              RenderValidationMode validation = RenderValidationMode::Full);

/// Scene-owned canonical CPU data and each writable snapshot's pending publication pages.
struct SnapshotPublicationFailure {
    uint32_t AfterRetainedOwners{UINT32_MAX};
    uint32_t AfterCopiedTables{UINT32_MAX};
    uint32_t AfterCopiedEntries{UINT32_MAX};
};

class SceneRenderState {
public:
    SceneRenderState();
    ~SceneRenderState() noexcept;
    SceneRenderState(const SceneRenderState&) = delete;
    SceneRenderState& operator=(const SceneRenderState&) = delete;
    /// An explicit epoch belongs to one publication target and one frame owner sink. New targets require a new epoch.
    /// Failed publication retries require unchanged owner sources; advance the epoch after late authoring edits.
    bool Publish(const Scene& scene, RenderSceneSnapshot& out, vector<StreamingAssetRefAny>& retainedAssets,
                 RenderValidationMode validation, std::optional<uint64_t> serial = std::nullopt);
    Nullable<shared_ptr<const RenderSceneSnapshot>> PrepareShared(const Scene& scene, uint64_t serial, uint32_t flight,
                                                                  vector<StreamingAssetRefAny>& retainedAssets, RenderValidationMode validation);
    /// Deterministic one-shot failure injection at the publication boundary, for lifecycle tests.
    void FailNextPublicationForTesting(SnapshotPublicationFailure failure) noexcept;
    SceneRenderStateMemoryStats GetMemoryStats() const noexcept;

private:
    struct Impl;
    unique_ptr<Impl> _impl;
};

/// GT publisher facade; all compilation state belongs to the supplied Scene.
class RenderSceneSnapshotBuilder {
public:
    bool Build(const Scene& scene, RenderSceneSnapshot& out, vector<StreamingAssetRefAny>& retainedAssets,
               RenderValidationMode validation = RenderValidationMode::Full,
               std::optional<uint64_t> serial = std::nullopt);
};

}  // namespace radray
