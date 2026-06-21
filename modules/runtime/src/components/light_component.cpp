#include <radray/runtime/components/light_component.h>

#include <algorithm>

#include <radray/runtime/game_framework/actor.h>
#include <radray/runtime/game_framework/world.h>
#include <radray/runtime/renderer/scene.h>

namespace radray {

LightComponent::~LightComponent() noexcept = default;

RuntimeTypeId LightComponent::GetTypeId() const noexcept {
    return runtime_type_id_v<LightComponent>;
}

void LightComponent::SetLightType(LightType type) noexcept {
    _type = type;
    if (_sceneProxy != nullptr) {
        _sceneProxy->SetLightType(type);
    }
}

void LightComponent::SetDirection(const Eigen::Vector3f& direction) noexcept {
    _direction = direction;
    if (_sceneProxy != nullptr) {
        _sceneProxy->SetDirection(direction);
    }
}

void LightComponent::SetColor(const Eigen::Vector3f& color) noexcept {
    _color = color;
    if (_sceneProxy != nullptr) {
        _sceneProxy->SetColor(color);
    }
}

void LightComponent::SetIntensity(float intensity) noexcept {
    _intensity = intensity;
    if (_sceneProxy != nullptr) {
        _sceneProxy->SetIntensity(intensity);
    }
}

void LightComponent::SetCastShadow(bool castShadow) noexcept {
    _castShadow = castShadow;
    if (_sceneProxy != nullptr) {
        _sceneProxy->SetCastShadow(castShadow);
    }
}

void LightComponent::SetShadowBias(float depthBias, float normalBias) noexcept {
    _shadowDepthBias = std::max(depthBias, 0.0f);
    _shadowNormalBias = std::max(normalBias, 0.0f);
    if (_sceneProxy != nullptr) {
        _sceneProxy->SetShadowDepthBias(_shadowDepthBias);
        _sceneProxy->SetShadowNormalBias(_shadowNormalBias);
    }
}

void LightComponent::SetRange(float range) noexcept {
    _range = std::max(range, 0.0f);
    if (_sceneProxy != nullptr) {
        _sceneProxy->SetRange(_range);
    }
}

void LightComponent::SetSpotAngles(float innerAngleRadians, float outerAngleRadians) noexcept {
    // Outer cone must not collapse below the inner cone; clamp both into a sane (0, ~89deg] range.
    constexpr float kMaxHalfAngle = 1.5533431f;  // ~89 deg
    _spotOuterAngle = std::clamp(outerAngleRadians, 0.001f, kMaxHalfAngle);
    _spotInnerAngle = std::clamp(innerAngleRadians, 0.0f, _spotOuterAngle);
    if (_sceneProxy != nullptr) {
        _sceneProxy->SetSpotInnerAngle(_spotInnerAngle);
        _sceneProxy->SetSpotOuterAngle(_spotOuterAngle);
    }
}

void LightComponent::OnRegister() {
    SceneComponent::OnRegister();

    auto proxy = make_unique<LightSceneProxy>();
    _sceneProxy = proxy.get();
    proxy->SetWorldMatrix(GetWorldMatrix());
    PushParametersToProxy();
    GetScene()->AddLight(std::move(proxy));
}

void LightComponent::OnUnregister() {
    if (_sceneProxy != nullptr) {
        GetScene()->RemoveLight(_sceneProxy);
        _sceneProxy = nullptr;
    }
    SceneComponent::OnUnregister();
}

void LightComponent::OnTransformChanged() {
    if (_sceneProxy != nullptr) {
        _sceneProxy->SetWorldMatrix(GetWorldMatrix());
    }
}

Scene* LightComponent::GetScene() const noexcept {
    return GetWorld().Get()->GetScene();
}

void LightComponent::PushParametersToProxy() noexcept {
    if (_sceneProxy == nullptr) {
        return;
    }
    _sceneProxy->SetLightType(_type);
    _sceneProxy->SetDirection(_direction);
    _sceneProxy->SetColor(_color);
    _sceneProxy->SetIntensity(_intensity);
    _sceneProxy->SetCastShadow(_castShadow);
    _sceneProxy->SetShadowDepthBias(_shadowDepthBias);
    _sceneProxy->SetShadowNormalBias(_shadowNormalBias);
    _sceneProxy->SetRange(_range);
    _sceneProxy->SetSpotInnerAngle(_spotInnerAngle);
    _sceneProxy->SetSpotOuterAngle(_spotOuterAngle);
}

}  // namespace radray
