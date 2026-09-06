#pragma once

#include <radray/runtime/material.h>
#include <radray/runtime/render_framework/light_scene_proxy.h>
#include <radray/runtime/render_framework/mesh_batch.h>
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
    // Peak vector capacities, measured in elements across reuse cycles.
    size_t PrimitiveHighWatermark{0}, BatchHighWatermark{0}, MaterialHighWatermark{0}, LightHighWatermark{0};
};

/// Per-flight values. Geometry/texture payloads and programs must outlive flight retirement.
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

}  // namespace radray
