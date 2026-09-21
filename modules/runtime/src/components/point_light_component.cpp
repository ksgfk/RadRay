#include <radray/runtime/components/point_light_component.h>

#include <algorithm>
#include <cmath>

namespace radray {

PointLightComponent::~PointLightComponent() noexcept = default;

LightData PointLightComponent::CaptureLightState() const noexcept {
    return PointLightData{CaptureCommon(), CapturePointParameters()};
}

PointLightParameters PointLightComponent::CapturePointParameters() const noexcept {
    PointLightParameters point;
    point.Position = GetLightPosition().head<3>();
    point.Direction = GetLightDirection();
    point.AttenuationRadius = _attenuationRadius;
    point.FalloffExponent = _lightFalloffExponent;
    point.SourceRadius = _sourceRadius;
    point.SoftSourceRadius = _softSourceRadius;
    point.SourceLength = _sourceLength;
    point.ShadowDepthBias = _shadowDepthBias;
    point.ShadowNormalBias = _shadowNormalBias;
    point.InverseSquaredFalloff = _useInverseSquaredFalloff;
    return point;
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
