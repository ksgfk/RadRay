#pragma once

#include <cstdint>

#include <radray/basic_math.h>

namespace radray {

enum class LightType : uint32_t {
    Directional,
    Point,
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

    const Eigen::Vector3f& GetPosition() const noexcept { return _position; }
    void SetPosition(const Eigen::Vector3f& position) noexcept { _position = position; }

    void SetWorldMatrix(const Eigen::Matrix4f& matrix) noexcept {
        _position = matrix.block<3, 1>(0, 3);
    }

private:
    LightType _type{LightType::Point};
    Eigen::Vector3f _direction{0.0f, -1.0f, 0.0f};
    Eigen::Vector3f _color{1.0f, 1.0f, 1.0f};
    float _intensity{1.0f};
    Eigen::Vector3f _position{Eigen::Vector3f::Zero()};
};

}  // namespace radray
