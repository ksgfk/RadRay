#include <radray/runtime/components/light_component.h>

#include <algorithm>
#include <radray/runtime/render_scene/scene_writer.h>

namespace radray {

LightComponent::~LightComponent() noexcept = default;

Eigen::Vector4f LightComponent::GetLightPosition() const noexcept {
    Eigen::Vector3f worldLocation = GetWorldLocation();
    return Eigen::Vector4f{worldLocation.x(), worldLocation.y(), worldLocation.z(), 1.0f};
}

Eigen::Vector3f LightComponent::GetLightDirection() const noexcept {
    Eigen::Vector3f direction = GetWorldRotation() * Eigen::Vector3f::UnitZ();
    if (direction.squaredNorm() <= 1e-8f) {
        return Eigen::Vector3f::UnitZ();
    }
    return direction.normalized();
}

void LightComponent::SetIntensity(float intensity) noexcept {
    CheckCanModify();
    if (_intensity == std::max(intensity, 0.0f)) return;
    _intensity = std::max(intensity, 0.0f);
    MarkRenderDynamicDataDirty();
}

void LightComponent::SetLightColor(const Eigen::Vector3f& color) noexcept {
    CheckCanModify();
    if (_lightColor == color.cwiseMax(Eigen::Vector3f::Zero())) return;
    _lightColor = color.cwiseMax(Eigen::Vector3f::Zero());
    MarkRenderDynamicDataDirty();
}

void LightComponent::SetAffectsWorld(bool affectsWorld) noexcept {
    CheckCanModify();
    if (_affectsWorld == affectsWorld) return;
    _affectsWorld = affectsWorld;
    MarkRenderDynamicDataDirty();
}

void LightComponent::SetCastShadow(bool castShadow) noexcept {
    CheckCanModify();
    if (_castShadow == castShadow) return;
    _castShadow = castShadow;
    MarkRenderDynamicDataDirty();
}

void LightComponent::CreateRenderState(SceneWriter& writer) {
    _lightId = writer.CreateLight();
    MarkRenderStateDirty();
}
void LightComponent::DestroyRenderState(SceneWriter& writer) {
    writer.RemoveLight(_lightId);
    _lightId = {};
}
void LightComponent::CollectRenderUpdates(SceneWriter& writer, RenderDirtyFlags) { writer.SetLight(CaptureLightState()); }
void LightComponent::OnTransformChanged() { MarkRenderTransformDirty(); }
LightData LightComponent::CaptureLightState() const noexcept {
    LightCommonData common;
    common.Id = _lightId;
    common.Color = _lightColor;
    common.Intensity = _intensity;
    common.AffectsWorld = _affectsWorld;
    common.CastShadow = _castShadow;
    const auto direction = GetLightDirection();
    switch (GetLightType()) {
        case LightType::Directional: return DirectionalLightData{common, direction};
        case LightType::Point: return PointLightData{common, {GetLightPosition().head<3>(), direction}};
        case LightType::Spot: return SpotLightData{common, {GetLightPosition().head<3>(), direction}};
        case LightType::Rect: return RectLightData{common, GetLightPosition().head<3>(), direction};
        default: RADRAY_ABORT("Invalid light type");
    }
}

}  // namespace radray
