#pragma once

#include <radray/inline_vector.h>
#include <radray/runtime/material.h>
#include <radray/runtime/render_framework/light_scene_proxy.h>
#include <radray/runtime/render_framework/mesh_batch.h>
#include <radray/runtime/render_framework/primitive_scene_proxy.h>
#include <radray/runtime/render_framework/render_bounds.h>

namespace radray {

class Scene;

struct RenderPrimitiveData {
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
    uint64_t MaterialBytesCopied{0}, ScratchEntriesCreated{0}, CpuNanoseconds{0};
    uint64_t PrimitiveStructuresRebuilt{0}, PrimitiveStructuresReused{0};
    uint64_t PrimitiveBoundsRebuilt{0}, PrimitiveBoundsReused{0};
    uint64_t MaterialsRebuilt{0}, MaterialsReused{0};
    // Peak vector capacities, measured in elements across reuse cycles.
    size_t PrimitiveHighWatermark{0}, BatchHighWatermark{0}, MaterialHighWatermark{0}, LightHighWatermark{0};
};

/// Per-flight values. Geometry/texture payloads and programs must outlive flight retirement.
/// Primitive values are builder-owned; ResetForReuse discards their materialization state.
struct RenderSceneSnapshot {
    vector<RenderPrimitiveData> Primitives;
    vector<MeshBatch> MeshBatches;
    vector<MaterialRenderData> Materials;
    vector<RenderLightData> Lights;
    RenderSceneSnapshotStats Stats;

    void ResetForReuse() noexcept;
};

/// Game thread only, after acquiring a writable flight. Failure publishes an empty snapshot.
bool BuildRenderSceneSnapshot(const Scene& scene, RenderSceneSnapshot& out, vector<StreamingAssetRefAny>& retainedAssets);

/// Game-thread cache; never published to the renderer. Geometry cache hits require proxy generation
/// and an explicit nonzero revision. Material values are independently versioned in each flight.
class RenderSceneSnapshotBuilder {
public:
    bool Build(const Scene& scene, RenderSceneSnapshot& out, vector<StreamingAssetRefAny>& retainedAssets);

private:
    struct Entry {
        uint64_t Epoch{0};
        std::optional<uint32_t> Index;
    };
    struct MaterialEntry {
        uint64_t Epoch{0}, StorageEpoch{0};
        std::optional<uint32_t> Index;
        size_t StorageIndex{0};
        // No asset ownership or renderer access. Invalid values are rechecked on every GT build.
        MaterialRenderData Unpublished;
    };
    enum class SectionStatus : uint8_t { Valid,
                                         MissingGeometry,
                                         EmptyDraw,
                                         InvalidDrawRange };
    struct Section {
        MeshDrawArgs Draw;
        SectionStatus Status{SectionStatus::MissingGeometry};
    };
    struct PrimitiveEntry {
        uint64_t Generation{0}, Revision{0};
        AxisAlignedBounds LocalBounds;
        InlineVector<Section, 2> Sections;
    };
    vector<PrimitiveEntry> _primitives;
    unordered_map<uint64_t, MaterialEntry> _materials;
    vector<MaterialRenderData> _materialScratch;
    unordered_map<ShaderProgram*, Entry> _programs;
    uint64_t _epoch{0};
};

}  // namespace radray
