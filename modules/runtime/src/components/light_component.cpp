#include <radray/runtime/components/light_component.h>

#include <algorithm>


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
    _intensity = std::max(intensity, 0.0f);
}

void LightComponent::SetLightColor(const Eigen::Vector3f& color) noexcept {
    _lightColor = color.cwiseMax(Eigen::Vector3f::Zero());
}

void LightComponent::SetAffectsWorld(bool affectsWorld) noexcept {
    _affectsWorld = affectsWorld;
}

void LightComponent::SetCastShadow(bool castShadow) noexcept {
    _castShadow = castShadow;
}

}  // namespace radray
