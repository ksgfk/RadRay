#include <radray/runtime/components/point_light_component.h>

#include <algorithm>
#include <cmath>

namespace radray {

PointLightComponent::~PointLightComponent() noexcept = default;

LightStateUpdate PointLightComponent::CaptureLightState() const noexcept {
    auto result = LightComponent::CaptureLightState();
    result.AttenuationRadius = _attenuationRadius;
    result.FalloffExponent = _lightFalloffExponent;
    result.SourceRadius = _sourceRadius;
    result.SoftSourceRadius = _softSourceRadius;
    result.SourceLength = _sourceLength;
    result.ShadowDepthBias = _shadowDepthBias;
    result.ShadowNormalBias = _shadowNormalBias;
    result.InverseSquaredFalloff = _useInverseSquaredFalloff;
    return result;
}

void PointLightComponent::SetAttenuationRadius(float radius) noexcept {
    CheckCanModify();
    if (!std::isfinite(radius) || radius < 0) return;
    if (_attenuationRadius == std::max(radius, 0.0f)) return;
    _attenuationRadius = std::max(radius, 0.0f);
    MarkRenderDynamicDataDirty();
}

float PointLightComponent::GetInvAttenuationRadius() const noexcept {
    return _attenuationRadius > 0.0f ? 1.0f / _attenuationRadius : 0.0f;
}

void PointLightComponent::SetUseInverseSquaredFalloff(bool value) noexcept {
    CheckCanModify();
    if (_useInverseSquaredFalloff == value) return;
    _useInverseSquaredFalloff = value;
    MarkRenderDynamicDataDirty();
}

void PointLightComponent::SetLightFalloffExponent(float exponent) noexcept {
    CheckCanModify();
    if (_lightFalloffExponent == std::max(exponent, 0.0f)) return;
    _lightFalloffExponent = std::max(exponent, 0.0f);
    MarkRenderDynamicDataDirty();
}

void PointLightComponent::SetSourceRadius(float radius) noexcept {
    CheckCanModify();
    if (_sourceRadius == std::max(radius, 0.0f)) return;
    _sourceRadius = std::max(radius, 0.0f);
    MarkRenderDynamicDataDirty();
}

void PointLightComponent::SetSoftSourceRadius(float radius) noexcept {
    CheckCanModify();
    if (_softSourceRadius == std::max(radius, 0.0f)) return;
    _softSourceRadius = std::max(radius, 0.0f);
    MarkRenderDynamicDataDirty();
}

void PointLightComponent::SetSourceLength(float length) noexcept {
    CheckCanModify();
    if (_sourceLength == std::max(length, 0.0f)) return;
    _sourceLength = std::max(length, 0.0f);
    MarkRenderDynamicDataDirty();
}

void PointLightComponent::SetShadowDepthBias(float bias) noexcept {
    CheckCanModify();
    if (_shadowDepthBias == std::max(bias, 0.0f)) return;
    _shadowDepthBias = std::max(bias, 0.0f);
    MarkRenderDynamicDataDirty();
}

void PointLightComponent::SetShadowNormalBias(float bias) noexcept {
    CheckCanModify();
    if (_shadowNormalBias == std::max(bias, 0.0f)) return;
    _shadowNormalBias = std::max(bias, 0.0f);
    MarkRenderDynamicDataDirty();
}

}  // namespace radray
