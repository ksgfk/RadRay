#pragma once

#include <radray/runtime/render_framework/render_scene_snapshot.h>
#include <radray/runtime/render_framework/render_view.h>

namespace radray {

struct FrustumPlane {
    Eigen::Vector3f Normal{Eigen::Vector3f::Zero()};
    float Distance{0};
};
struct ViewFrustum {
    array<FrustumPlane, 6> Planes;
    uint32_t ActivePlaneMask{0};
};

std::optional<ViewFrustum> ExtractViewFrustum(const Eigen::Matrix4f& viewProjection) noexcept;
bool IntersectsFrustum(const ViewFrustum& frustum, const AxisAlignedBounds& bounds) noexcept;
bool IntersectsFrustum(const ViewFrustum& frustum, const SphereBounds& bounds) noexcept;

struct CullingParameters {
    Nullable<const RenderSceneSnapshot*> Scene{nullptr};
    Nullable<const ResolvedRenderView*> View{nullptr};
    uint32_t LayerMask{0xffffffffu};
    /// Optional conservative, unjittered frustum; draw sorting still uses View.
    std::optional<Eigen::Matrix4f> ViewProjection{};
};
struct VisiblePrimitive {
    RenderPrimitiveIndex Primitive{0};
    float ViewDepth{0};
};
struct VisibleLight {
    uint32_t Light{0};
    float DistanceSquared{0};
};
struct CullingStats {
    uint64_t InputPrimitives{0}, LayerRejected{0}, FrustumRejected{0}, InvalidBoundsVisible{0}, VisiblePrimitives{0};
    uint64_t InputLights{0}, LightLayerRejected{0}, LightFrustumRejected{0}, InvalidLightBounds{0}, UnsupportedLights{0}, VisibleLights{0};
    uint64_t InvalidLightParameters{0};
    uint64_t InvalidDepth{0};
    double CpuMilliseconds{0};
    bool Valid{false};
};
/// Scene and View are borrowed from the current flight and must remain unchanged during list building.
struct CullingResults {
    Nullable<const RenderSceneSnapshot*> Scene{nullptr};
    Nullable<const ResolvedRenderView*> View{nullptr};
    vector<VisiblePrimitive> Primitives;
    vector<VisibleLight> Lights;
    CullingStats Stats;

    void ResetForReuse() noexcept;
};

/// Uses column vectors and clip depth [0,w]. On failure, results are empty and invalid.
bool Cull(const CullingParameters& parameters, CullingResults& out) noexcept;

}  // namespace radray
