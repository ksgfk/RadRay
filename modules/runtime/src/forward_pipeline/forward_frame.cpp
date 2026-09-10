#include "forward_frame.h"

#include <algorithm>
#include <cmath>
#include <radray/logger.h>
#include <radray/profiler.h>
#include <radray/runtime/components/camera_component.h>

namespace radray::forward_detail {
namespace {
constexpr uint32_t kMaxDirectionalLights = 8;
constexpr uint32_t kMaxPointLights = 8;
}  // namespace

RenderViewDesc CollectRenderView(const CameraComponent& camera) {
    RenderViewDesc view;
    view.Name = "Forward Camera";
    view.WorldToView = camera.ComputeViewMatrix();
    view.WorldPosition = camera.GetEyePosition();
    view.Projection = PerspectiveProjectionDesc{camera.GetFovY(), camera.GetNearZ(), camera.GetFarZ()};
    return view;
}

// Runs once per visible primitive per frame; written against raw components so unoptimized builds do
// not pay an out-of-line call per Eigen operator.
Eigen::Matrix4f MakeNormalToWorld(const Eigen::Matrix4f& localToWorld) {
    Eigen::Matrix4f result = Eigen::Matrix4f::Zero();
    // Column-major: element (r, c) at m[c * 4 + r]. l[c][r] is column c of the linear part.
    const float* m = localToWorld.data();
    float l[3][3];
    float scale = 0.0f;
    bool finite = true;
    for (int c = 0; c < 3; ++c) {
        for (int r = 0; r < 3; ++r) {
            const float v = m[c * 4 + r];
            l[c][r] = v;
            finite = finite && std::isfinite(v);
            scale = std::max(scale, std::abs(v));
        }
    }
    if (!finite || scale == 0.0f) {
        return result;
    }
    const float inv = 1.0f / scale;
    for (auto& column : l)
        for (float& v : column) v *= inv;
    const auto cross = [](const float* a, const float* b, float* out) {
        out[0] = a[1] * b[2] - a[2] * b[1];
        out[1] = a[2] * b[0] - a[0] * b[2];
        out[2] = a[0] * b[1] - a[1] * b[0];
    };
    float cofactor[3][3];
    cross(l[1], l[2], cofactor[0]);
    cross(l[2], l[0], cofactor[1]);
    cross(l[0], l[1], cofactor[2]);
    const float det = l[0][0] * cofactor[0][0] + l[0][1] * cofactor[0][1] + l[0][2] * cofactor[0][2];
    if (det < 0.0f) {
        for (auto& column : cofactor)
            for (float& v : column) v = -v;
    }
    float normalScale = 0.0f;
    for (const auto& column : cofactor)
        for (const float v : column) normalScale = std::max(normalScale, std::abs(v));
    if (normalScale > 0.0f) {
        float* out = result.data();
        const float invNormal = 1.0f / normalScale;
        for (int c = 0; c < 3; ++c)
            for (int r = 0; r < 3; ++r) out[c * 4 + r] = cofactor[c][r] * invNormal;
    }
    return result;
}

void FreezeObjectData(const RenderSceneSnapshot& scene, PackedCBufferTable& out) {
    RADRAY_PROFILE_SCOPE_N("FreezeObjectData");
    out.Reset(sizeof(Forward_ObjectData), scene.Primitives.size());
    for (size_t index = 0; index < scene.Primitives.size(); ++index) {
        const Eigen::Matrix4f& localToWorld = scene.Primitives[index].LocalToWorld;
        auto* row = AsCBuffer<Forward_ObjectData>(out.Row(index));
        row->LocalToWorld = localToWorld;
        row->NormalToWorld = MakeNormalToWorld(localToWorld);
        row->PreviousLocalToWorld = localToWorld;
        row->MotionValid = 0;
    }
}

void FillViewParameters(
    Forward_ViewData& out,
    const CullingResults& culling,
    const ResolvedRenderView& view,
    bool& lightOverflowWarned, bool localLightsFromPass) {
    RADRAY_PROFILE_SCOPE_N("FillViewParameters");
    struct SelectedLight {
        LightRenderParameters Parameters;
        float Radius;
        float DistanceSquared;
    };
    out = {};
    out.ViewProj = view.ViewProjection;
    out.PreviousViewProj = view.PreviousViewValid ? view.PreviousViewProjection : view.ViewProjection;
    const Eigen::Vector3f eye = view.WorldPosition;
    out.EyePosition = Eigen::Vector4f{eye.x(), eye.y(), eye.z(), 1.0f};

    vector<SelectedLight> directional;
    vector<SelectedLight> points;
    for (const VisibleLight& visible : culling.Lights) {
        const auto& light = culling.Scene->Lights[visible.Light];
        SelectedLight selected{
            .Parameters = light.Parameters,
            .Radius = light.WorldBounds.Radius,
            .DistanceSquared = visible.DistanceSquared};
        if (light.Type == LightType::Directional) {
            directional.push_back(selected);
        } else if (light.Type == LightType::Point && !localLightsFromPass) {
            points.push_back(selected);
        }
    }
    const auto sortByDistance = [](vector<SelectedLight>& lights) {
        std::stable_sort(
            lights.begin(),
            lights.end(),
            [](const SelectedLight& lhs, const SelectedLight& rhs) noexcept {
                return lhs.DistanceSquared < rhs.DistanceSquared;
            });
    };
    sortByDistance(directional);
    sortByDistance(points);
    if ((directional.size() > kMaxDirectionalLights ||
         points.size() > kMaxPointLights) &&
        !lightOverflowWarned) {
        RADRAY_WARN_LOG("forward pipeline light limit exceeded; nearest supported lights are used");
        lightOverflowWarned = true;
    }
    directional.resize(std::min<size_t>(directional.size(), kMaxDirectionalLights));
    points.resize(std::min<size_t>(points.size(), kMaxPointLights));
    out.DirectionalLightCount = static_cast<uint32_t>(directional.size());
    out.PointLightCount = static_cast<uint32_t>(points.size());
    for (uint32_t index = 0; index < directional.size(); ++index) {
        const LightRenderParameters& light = directional[index].Parameters;
        out.DirectionalLights[index].Direction =
            Eigen::Vector4f{light.Direction.x(), light.Direction.y(), light.Direction.z(), 0.0f};
        out.DirectionalLights[index].Irradiance = Eigen::Vector4f{
            light.Color.x() * light.DiffuseScale,
            light.Color.y() * light.DiffuseScale,
            light.Color.z() * light.DiffuseScale,
            0.0f};
    }
    for (uint32_t index = 0; index < points.size(); ++index) {
        const SelectedLight& selected = points[index];
        const LightRenderParameters& light = selected.Parameters;
        out.PointLights[index].Position = Eigen::Vector4f{
            light.WorldPosition.x(), light.WorldPosition.y(), light.WorldPosition.z(), selected.Radius};
        out.PointLights[index].Intensity = Eigen::Vector4f{
            light.Color.x() * light.DiffuseScale,
            light.Color.y() * light.DiffuseScale,
            light.Color.z() * light.DiffuseScale,
            0.0f};
    }
}

}  // namespace radray::forward_detail
