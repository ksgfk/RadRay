#include <radray/runtime/components/point_light_component.h>

#include <algorithm>

#include <radray/runtime/render_framework/point_light_scene_proxy.h>

namespace radray {

PointLightComponent::~PointLightComponent() noexcept = default;

RuntimeTypeId PointLightComponent::GetTypeId() const noexcept {
    return runtime_type_id_v<PointLightComponent>;
}

unique_ptr<LightSceneProxy> PointLightComponent::CreateSceneProxy() const {
    return make_unique<PointLightSceneProxy>(*this);
}

void PointLightComponent::SetAttenuationRadius(float radius) noexcept {
    _attenuationRadius = std::max(radius, 0.0f);
    MarkRenderStateDirty();
}

float PointLightComponent::GetInvAttenuationRadius() const noexcept {
    return _attenuationRadius > 0.0f ? 1.0f / _attenuationRadius : 0.0f;
}

void PointLightComponent::SetUseInverseSquaredFalloff(bool value) noexcept {
    _useInverseSquaredFalloff = value;
    MarkRenderStateDirty();
}

void PointLightComponent::SetLightFalloffExponent(float exponent) noexcept {
    _lightFalloffExponent = std::max(exponent, 0.0f);
    MarkRenderStateDirty();
}

void PointLightComponent::SetSourceRadius(float radius) noexcept {
    _sourceRadius = std::max(radius, 0.0f);
    MarkRenderStateDirty();
}

void PointLightComponent::SetSoftSourceRadius(float radius) noexcept {
    _softSourceRadius = std::max(radius, 0.0f);
    MarkRenderStateDirty();
}

void PointLightComponent::SetSourceLength(float length) noexcept {
    _sourceLength = std::max(length, 0.0f);
    MarkRenderStateDirty();
}

void PointLightComponent::SetShadowDepthBias(float bias) noexcept {
    _shadowDepthBias = std::max(bias, 0.0f);
    MarkRenderStateDirty();
}

void PointLightComponent::SetShadowNormalBias(float bias) noexcept {
    _shadowNormalBias = std::max(bias, 0.0f);
    MarkRenderStateDirty();
}

}  // namespace radray
