#include <radray/runtime/components/light_component.h>

#include <algorithm>

#include <radray/runtime/game_framework/actor.h>
#include <radray/runtime/game_framework/world.h>
#include <radray/runtime/render/scene.h>

namespace radray {

LightComponent::~LightComponent() noexcept = default;

RuntimeTypeId LightComponent::GetTypeId() const noexcept {
    return runtime_type_id_v<LightComponent>;
}

void LightComponent::SetLightType(srp::LightType type) noexcept {
    _type = type;
    if (_light != nullptr) {
        _light->Type = type;
    }
}

void LightComponent::SetDirection(const Eigen::Vector3f& direction) noexcept {
    _direction = direction;
    if (_light != nullptr) {
        _light->Direction = direction;
    }
}

void LightComponent::SetColor(const Eigen::Vector3f& color) noexcept {
    _color = color;
    if (_light != nullptr) {
        _light->Color = color;
    }
}

void LightComponent::SetIntensity(float intensity) noexcept {
    _intensity = intensity;
    if (_light != nullptr) {
        _light->Intensity = intensity;
    }
}

void LightComponent::SetCastShadow(bool castShadow) noexcept {
    _castShadow = castShadow;
    if (_light != nullptr) {
        _light->CastShadow = castShadow;
    }
}

void LightComponent::SetShadowBias(float depthBias, float normalBias) noexcept {
    _shadowDepthBias = std::max(depthBias, 0.0f);
    _shadowNormalBias = std::max(normalBias, 0.0f);
    if (_light != nullptr) {
        _light->ShadowDepthBias = _shadowDepthBias;
        _light->ShadowNormalBias = _shadowNormalBias;
    }
}

void LightComponent::SetRange(float range) noexcept {
    _range = std::max(range, 0.0f);
    if (_light != nullptr) {
        _light->Range = _range;
    }
}

void LightComponent::SetSpotAngles(float innerAngleRadians, float outerAngleRadians) noexcept {
    // Outer cone must not collapse below the inner cone; clamp both into a sane (0, ~89deg] range.
    constexpr float kMaxHalfAngle = 1.5533431f;  // ~89 deg
    _spotOuterAngle = std::clamp(outerAngleRadians, 0.001f, kMaxHalfAngle);
    _spotInnerAngle = std::clamp(innerAngleRadians, 0.0f, _spotOuterAngle);
    if (_light != nullptr) {
        _light->SpotInnerAngle = _spotInnerAngle;
        _light->SpotOuterAngle = _spotOuterAngle;
    }
}

void LightComponent::OnRegister() {
    SceneComponent::OnRegister();

    _light = GetScene()->AddLight();
    PushParametersToLight();
    _light->Position = GetWorldMatrix().block<3, 1>(0, 3);
}

void LightComponent::OnUnregister() {
    if (_light != nullptr) {
        GetScene()->RemoveLight(_light);
        _light = nullptr;
    }
    SceneComponent::OnUnregister();
}

void LightComponent::OnTransformChanged() {
    if (_light != nullptr) {
        _light->Position = GetWorldMatrix().block<3, 1>(0, 3);
    }
}

srp::Scene* LightComponent::GetScene() const noexcept {
    return GetWorld().Get()->GetScene();
}

void LightComponent::PushParametersToLight() noexcept {
    if (_light == nullptr) {
        return;
    }
    _light->Type = _type;
    _light->Direction = _direction;
    _light->Color = _color;
    _light->Intensity = _intensity;
    _light->CastShadow = _castShadow;
    _light->ShadowDepthBias = _shadowDepthBias;
    _light->ShadowNormalBias = _shadowNormalBias;
    _light->Range = _range;
    _light->SpotInnerAngle = _spotInnerAngle;
    _light->SpotOuterAngle = _spotOuterAngle;
}

}  // namespace radray
