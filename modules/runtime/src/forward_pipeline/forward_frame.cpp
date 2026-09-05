#include "forward_frame.h"

#include <algorithm>
#include <radray/logger.h>
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

bool FillViewParameters(
    ShaderParameterStorage& storage,
    const CullingResults& culling,
    const ResolvedRenderView& view,
    bool& lightOverflowWarned) {
    struct SelectedLight {
        LightRenderParameters Parameters;
        float Radius;
        float DistanceSquared;
    };
    if (!storage.SetMatrix4x4(
            "ViewProj",
            view.ViewProjection)) {
        return false;
    }
    const Eigen::Vector3f eye = view.WorldPosition;
    if (!storage.SetFloat4(
            "EyePosition",
            Eigen::Vector4f{eye.x(), eye.y(), eye.z(), 1.0f})) {
        return false;
    }

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
        } else if (light.Type == LightType::Point) {
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
    if (!storage.SetUInt(
            "DirectionalLightCount",
            static_cast<uint32_t>(directional.size())) ||
        !storage.SetUInt(
            "PointLightCount",
            static_cast<uint32_t>(points.size()))) {
        return false;
    }
    for (uint32_t index = 0; index < directional.size(); ++index) {
        const LightRenderParameters& light = directional[index].Parameters;
        if (!storage.SetFloat4(
                "Direction",
                Eigen::Vector4f{
                    light.Direction.x(),
                    light.Direction.y(),
                    light.Direction.z(),
                    0.0f},
                index) ||
            !storage.SetFloat4(
                "Irradiance",
                Eigen::Vector4f{
                    light.Color.x() * light.DiffuseScale,
                    light.Color.y() * light.DiffuseScale,
                    light.Color.z() * light.DiffuseScale,
                    0.0f},
                index)) {
            return false;
        }
    }
    for (uint32_t index = 0; index < points.size(); ++index) {
        const SelectedLight& selected = points[index];
        const LightRenderParameters& light = selected.Parameters;
        if (!storage.SetFloat4(
                "Position",
                Eigen::Vector4f{
                    light.WorldPosition.x(),
                    light.WorldPosition.y(),
                    light.WorldPosition.z(),
                    selected.Radius},
                index) ||
            !storage.SetFloat4(
                "Intensity",
                Eigen::Vector4f{
                    light.Color.x() * light.DiffuseScale,
                    light.Color.y() * light.DiffuseScale,
                    light.Color.z() * light.DiffuseScale,
                    0.0f},
                index)) {
            return false;
        }
    }
    return true;
}

}  // namespace radray::forward_detail
