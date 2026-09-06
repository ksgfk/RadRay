#include <radray/runtime/render_framework/culling.h>

#include <chrono>
#include <cmath>
#include <limits>

namespace radray {

std::optional<ViewFrustum> ExtractViewFrustum(const Eigen::Matrix4f& matrix) noexcept {
    if (!matrix.allFinite()) return std::nullopt;
    const array<Eigen::Vector4f, 6> planes{
        (matrix.row(3) + matrix.row(0)).transpose(), (matrix.row(3) - matrix.row(0)).transpose(),
        (matrix.row(3) + matrix.row(1)).transpose(), (matrix.row(3) - matrix.row(1)).transpose(),
        matrix.row(2).transpose(), (matrix.row(3) - matrix.row(2)).transpose()};
    ViewFrustum result;
    for (uint32_t index = 0; index < planes.size(); ++index) {
        const auto& plane = planes[index];
        const double length = plane.head<3>().cast<double>().norm();
        if (!plane.allFinite() || !std::isfinite(length)) return std::nullopt;
        if (length == 0) {
            // An infinite far plane imposes no constraint. Other degenerate planes are invalid.
            if (index == 5 && plane.w() >= 0) continue;
            return std::nullopt;
        }
        const Eigen::Vector4f normalized = (plane.cast<double>() / length).cast<float>();
        if (!normalized.allFinite()) return std::nullopt;
        result.Planes[index] = {normalized.head<3>(), normalized.w()};
        result.ActivePlaneMask |= 1u << index;
    }
    return result;
}

bool IntersectsFrustum(const ViewFrustum& frustum, const AxisAlignedBounds& bounds) noexcept {
    if (!bounds.IsFiniteValid()) return true;
    const auto center = bounds.Center().cast<double>().eval();
    const auto extents = bounds.Extents().cast<double>().eval();
    for (uint32_t index = 0; index < frustum.Planes.size(); ++index) {
        if (!(frustum.ActivePlaneMask & (1u << index))) continue;
        const auto& plane = frustum.Planes[index];
        const auto normal = plane.Normal.cast<double>().eval();
        const double distance = normal.dot(center) + plane.Distance;
        const double radius = normal.cwiseAbs().dot(extents);
        const double tolerance = 1e-6 * (1 + std::abs(distance) + radius);
        if (distance + radius < -tolerance) return false;
    }
    return true;
}

bool IntersectsFrustum(const ViewFrustum& frustum, const SphereBounds& bounds) noexcept {
    if (!bounds.IsFiniteValid()) return false;
    for (uint32_t index = 0; index < frustum.Planes.size(); ++index) {
        if (!(frustum.ActivePlaneMask & (1u << index))) continue;
        const auto& plane = frustum.Planes[index];
        const double distance = plane.Normal.cast<double>().dot(bounds.Center.cast<double>()) + plane.Distance;
        if (distance + bounds.Radius < -1e-6 * (1 + std::abs(distance) + bounds.Radius)) return false;
    }
    return true;
}

void CullingResults::ResetForReuse() noexcept {
    Scene = nullptr;
    View = nullptr;
    Primitives.clear();
    Lights.clear();
    Stats = {};
}

bool Cull(const CullingParameters& parameters, CullingResults& out) noexcept {
    out.ResetForReuse();
    const auto started = std::chrono::steady_clock::now();
    if (!parameters.Scene || !parameters.View || !parameters.View->View.allFinite() || !parameters.View->WorldPosition.allFinite()) return false;
    const auto frustum = ExtractViewFrustum(parameters.ViewProjection.value_or(parameters.View->ViewProjection));
    if (!frustum) return false;
    const auto& scene = *parameters.Scene.Get();
    const auto& view = *parameters.View.Get();
    if (scene.Primitives.size() > std::numeric_limits<uint32_t>::max() || scene.Lights.size() > std::numeric_limits<uint32_t>::max()) return false;
    out.Scene = &scene;
    out.View = &view;
    const uint32_t mask = view.LayerMask & parameters.LayerMask;
    out.Stats.InputPrimitives = scene.Primitives.size();
    for (uint32_t index = 0; index < scene.Primitives.size(); ++index) {
        const auto& primitive = scene.Primitives[index];
        if (!(primitive.LayerMask & mask)) {
            ++out.Stats.LayerRejected;
            continue;
        }
        const bool validBounds = primitive.WorldBounds.IsFiniteValid();
        if (!validBounds) {
            ++out.Stats.InvalidBoundsVisible;
        } else if (!primitive.DisableFrustumCulling && !IntersectsFrustum(*frustum, primitive.WorldBounds)) {
            ++out.Stats.FrustumRejected;
            continue;
        }
        const Eigen::Vector3f center = validBounds ? primitive.WorldBounds.Center() : Eigen::Vector3f{primitive.LocalToWorld.block<3, 1>(0, 3)};
        float depth = view.View.row(2).head<3>().dot(center) + view.View(2, 3);
        if (!std::isfinite(depth)) {
            depth = 0;
            ++out.Stats.InvalidDepth;
        }
        out.Primitives.push_back({index, depth});
    }
    out.Stats.InputLights = scene.Lights.size();
    for (uint32_t index = 0; index < scene.Lights.size(); ++index) {
        const auto& light = scene.Lights[index];
        if (!(light.LayerMask & mask)) {
            ++out.Stats.LightLayerRejected;
            continue;
        }
        if (light.Type == LightType::Point || light.Type == LightType::Spot) {
            if (!light.WorldBounds.IsFiniteValid() || light.WorldBounds.Radius == 0) {
                ++out.Stats.InvalidLightBounds;
                continue;
            }
            if (!IntersectsFrustum(*frustum, light.WorldBounds)) {
                ++out.Stats.LightFrustumRejected;
                continue;
            }
        } else if (light.Type != LightType::Directional) {
            ++out.Stats.UnsupportedLights;
            continue;
        }
        const auto& p = light.Parameters;
        const float scalars[]{p.InvRadius, p.FalloffExponent, p.SpecularScale, p.DiffuseScale, p.SourceRadius, p.SoftSourceRadius, p.SourceLength};
        const bool finite = p.WorldPosition.allFinite() && p.Color.allFinite() && p.Direction.allFinite() && p.Tangent.allFinite() &&
                            (p.Color * p.DiffuseScale).allFinite() &&
                            std::all_of(std::begin(scalars), std::end(scalars), [](float value) { return std::isfinite(value); });
        const float directionLength = p.Direction.squaredNorm();
        const bool direction = light.Type == LightType::Point || (std::isfinite(directionLength) && directionLength > 1e-12f);
        const bool cone = light.Type != LightType::Spot ||
                          (p.SpotAngles.allFinite() && p.SpotAngles.x() > 0 && p.SpotAngles.x() < 1 && p.SpotAngles.y() > 0 &&
                           p.SpotAngles.x() + 1 / p.SpotAngles.y() <= 1.000001f);
        if (!finite || !direction || !cone) {
            ++out.Stats.InvalidLightParameters;
            continue;
        }
        const double distance = (light.Parameters.WorldPosition.cast<double>() - view.WorldPosition.cast<double>()).squaredNorm();
        const float bounded = std::isfinite(distance) ? static_cast<float>(std::min(distance, double{std::numeric_limits<float>::max()})) : std::numeric_limits<float>::max();
        out.Lights.push_back({index, bounded});
    }
    out.Stats.VisiblePrimitives = out.Primitives.size();
    out.Stats.VisibleLights = out.Lights.size();
    out.Stats.Valid = true;
    out.Stats.CpuMilliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    return true;
}

}  // namespace radray
