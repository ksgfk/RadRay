#include <radray/runtime/components/point_light_component.h>

#include <algorithm>
#include <cmath>
#include <type_traits>

namespace radray {

PointLightComponent::~PointLightComponent() noexcept = default;

LightData PointLightComponent::CaptureLightState() const noexcept {
    auto result = LightComponent::CaptureLightState();
    std::visit([this](auto& light) {
        using T = std::decay_t<decltype(light)>;
        if constexpr (std::is_same_v<T, PointLightData> || std::is_same_v<T, SpotLightData>) {
            light.Point.AttenuationRadius = _attenuationRadius;
            light.Point.FalloffExponent = _lightFalloffExponent;
            light.Point.SourceRadius = _sourceRadius;
            light.Point.SoftSourceRadius = _softSourceRadius;
            light.Point.SourceLength = _sourceLength;
            light.Point.InverseSquaredFalloff = _useInverseSquaredFalloff;
            light.Common.ShadowDepthBias = _shadowDepthBias;
            light.Common.ShadowNormalBias = _shadowNormalBias;
        } else {
            RADRAY_ABORT("Point light component requires Point or Spot data");
        }
    },
               result);
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
