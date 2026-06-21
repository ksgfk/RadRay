#pragma once

#include <cstdint>

#include <radray/basic_math.h>

namespace radray {

enum class LightType : uint32_t {
    Directional,
    Point,
    Spot,
};

/// Render-side mirror of a LightComponent.
/// Holds light parameters in world space for per-view GPU light buffer builds.
class LightSceneProxy {
public:
    LightSceneProxy() noexcept = default;
    LightSceneProxy(const LightSceneProxy&) = delete;
    LightSceneProxy(LightSceneProxy&&) = delete;
    LightSceneProxy& operator=(const LightSceneProxy&) = delete;
    LightSceneProxy& operator=(LightSceneProxy&&) = delete;
    virtual ~LightSceneProxy() noexcept = default;

    LightType GetLightType() const noexcept { return _type; }
    void SetLightType(LightType type) noexcept { _type = type; }

    const Eigen::Vector3f& GetDirection() const noexcept { return _direction; }
    void SetDirection(const Eigen::Vector3f& direction) noexcept { _direction = direction; }

    const Eigen::Vector3f& GetColor() const noexcept { return _color; }
    void SetColor(const Eigen::Vector3f& color) noexcept { _color = color; }

    float GetIntensity() const noexcept { return _intensity; }
    void SetIntensity(float intensity) noexcept { _intensity = intensity; }

    bool GetCastShadow() const noexcept { return _castShadow; }
    void SetCastShadow(bool castShadow) noexcept { _castShadow = castShadow; }

    float GetShadowDepthBias() const noexcept { return _shadowDepthBias; }
    void SetShadowDepthBias(float depthBias) noexcept { _shadowDepthBias = depthBias; }

    float GetShadowNormalBias() const noexcept { return _shadowNormalBias; }
    void SetShadowNormalBias(float normalBias) noexcept { _shadowNormalBias = normalBias; }

    const Eigen::Vector3f& GetPosition() const noexcept { return _position; }
    void SetPosition(const Eigen::Vector3f& position) noexcept { _position = position; }

    /// Influence radius for point/spot lights (also the shadow projection far plane). <=0 disables attenuation cutoff.
    float GetRange() const noexcept { return _range; }
    void SetRange(float range) noexcept { _range = range; }

    /// Spot cone half-angles in radians. Inner = full intensity edge, Outer = zero intensity edge (must be >= inner).
    float GetSpotInnerAngle() const noexcept { return _spotInnerAngle; }
    void SetSpotInnerAngle(float angle) noexcept { _spotInnerAngle = angle; }

    float GetSpotOuterAngle() const noexcept { return _spotOuterAngle; }
    void SetSpotOuterAngle(float angle) noexcept { _spotOuterAngle = angle; }

    void SetWorldMatrix(const Eigen::Matrix4f& matrix) noexcept {
        _position = matrix.block<3, 1>(0, 3);
    }

private:
    LightType _type{LightType::Point};
    Eigen::Vector3f _direction{0.0f, -1.0f, 0.0f};
    Eigen::Vector3f _color{1.0f, 1.0f, 1.0f};
    float _intensity{1.0f};
    bool _castShadow{true};
    float _shadowDepthBias{1.0f};
    float _shadowNormalBias{1.0f};
    Eigen::Vector3f _position{Eigen::Vector3f::Zero()};
    float _range{10.0f};
    float _spotInnerAngle{0.5235988f};  // 30 deg
    float _spotOuterAngle{0.6981317f};  // 40 deg
};

}  // namespace radray
