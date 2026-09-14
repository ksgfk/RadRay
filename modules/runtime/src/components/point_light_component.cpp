#include <radray/runtime/components/point_light_component.h>

#include <algorithm>
#include <cmath>


namespace radray {

PointLightComponent::~PointLightComponent() noexcept = default;


void PointLightComponent::SetAttenuationRadius(float radius) noexcept {
    if (!std::isfinite(radius) || radius < 0) return;
    _attenuationRadius = std::max(radius, 0.0f);
}

float PointLightComponent::GetInvAttenuationRadius() const noexcept {
    return _attenuationRadius > 0.0f ? 1.0f / _attenuationRadius : 0.0f;
}

void PointLightComponent::SetUseInverseSquaredFalloff(bool value) noexcept {
    _useInverseSquaredFalloff = value;
}

void PointLightComponent::SetLightFalloffExponent(float exponent) noexcept {
    _lightFalloffExponent = std::max(exponent, 0.0f);
}

void PointLightComponent::SetSourceRadius(float radius) noexcept {
    _sourceRadius = std::max(radius, 0.0f);
}

void PointLightComponent::SetSoftSourceRadius(float radius) noexcept {
    _softSourceRadius = std::max(radius, 0.0f);
}

void PointLightComponent::SetSourceLength(float length) noexcept {
    _sourceLength = std::max(length, 0.0f);
}

void PointLightComponent::SetShadowDepthBias(float bias) noexcept {
    _shadowDepthBias = std::max(bias, 0.0f);
}

void PointLightComponent::SetShadowNormalBias(float bias) noexcept {
    _shadowNormalBias = std::max(bias, 0.0f);
}

}  // namespace radray
