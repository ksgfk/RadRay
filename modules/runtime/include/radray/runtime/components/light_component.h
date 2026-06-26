#pragma once

#include <radray/runtime/components/scene_component.h>
#include <radray/runtime/render/scene.h>

namespace radray {

namespace srp {
class Scene;
}  // namespace srp

class LightComponent;

template <>
struct RuntimeTypeTrait<LightComponent> {
    static constexpr RuntimeTypeId value{0x7e32d0c4, 0x9b0f, 0x43a2, 0x96, 0x91, 0x79, 0xdf, 0x3c, 0x31, 0x45, 0xb2};
};

/// Light source component registered into the render srp::Scene as an srp::Light.
class LightComponent : public SceneComponent {
public:
    LightComponent() noexcept = default;
    ~LightComponent() noexcept override;

    RuntimeTypeId GetTypeId() const noexcept override;

    void SetLightType(srp::LightType type) noexcept;
    void SetDirection(const Eigen::Vector3f& direction) noexcept;
    void SetColor(const Eigen::Vector3f& color) noexcept;
    void SetIntensity(float intensity) noexcept;
    void SetCastShadow(bool castShadow) noexcept;
    void SetShadowBias(float depthBias, float normalBias) noexcept;
    void SetRange(float range) noexcept;
    void SetSpotAngles(float innerAngleRadians, float outerAngleRadians) noexcept;

    srp::LightType GetLightType() const noexcept { return _type; }
    const Eigen::Vector3f& GetDirection() const noexcept { return _direction; }
    const Eigen::Vector3f& GetColor() const noexcept { return _color; }
    float GetIntensity() const noexcept { return _intensity; }
    bool GetCastShadow() const noexcept { return _castShadow; }
    float GetShadowDepthBias() const noexcept { return _shadowDepthBias; }
    float GetShadowNormalBias() const noexcept { return _shadowNormalBias; }
    float GetRange() const noexcept { return _range; }
    float GetSpotInnerAngle() const noexcept { return _spotInnerAngle; }
    float GetSpotOuterAngle() const noexcept { return _spotOuterAngle; }

    void OnRegister() override;
    void OnUnregister() override;

protected:
    void OnTransformChanged() override;

private:
    srp::Scene* GetScene() const noexcept;
    void PushParametersToLight() noexcept;

    srp::LightType _type{srp::LightType::Point};
    Eigen::Vector3f _direction{0.0f, -1.0f, 0.0f};
    Eigen::Vector3f _color{1.0f, 1.0f, 1.0f};
    float _intensity{1.0f};
    bool _castShadow{true};
    float _shadowDepthBias{1.0f};
    float _shadowNormalBias{1.0f};
    float _range{10.0f};
    float _spotInnerAngle{0.5235988f};  // 30 deg
    float _spotOuterAngle{0.6981317f};  // 40 deg

    /// non-owning, Scene holds the unique_ptr (stable handle).
    srp::Light* _light{nullptr};
};

}  // namespace radray
