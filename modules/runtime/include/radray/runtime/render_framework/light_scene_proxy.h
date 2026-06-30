#pragma once

#include <limits>

#include <radray/basic_math.h>

namespace radray {

class LightComponent;

enum class LightType : uint8_t {
    Directional,
    Point,
    Spot,
    Rect,
};

struct LightRenderParameters {
    Eigen::Vector3f WorldPosition{Eigen::Vector3f::Zero()};
    float InvRadius{0.0f};
    Eigen::Vector3f Color{Eigen::Vector3f::Ones()};
    float FalloffExponent{0.0f};
    Eigen::Vector3f Direction{Eigen::Vector3f::UnitZ()};
    float SpecularScale{1.0f};
    float DiffuseScale{1.0f};
    Eigen::Vector3f Tangent{Eigen::Vector3f::UnitX()};
    float SourceRadius{0.0f};
    Eigen::Vector2f SpotAngles{-2.0f, 1.0f};
    float SoftSourceRadius{0.0f};
    float SourceLength{0.0f};
};

class LightSceneProxy {
public:
    explicit LightSceneProxy(const LightComponent& component);
    LightSceneProxy(const LightSceneProxy&) = delete;
    LightSceneProxy(LightSceneProxy&&) = delete;
    LightSceneProxy& operator=(const LightSceneProxy&) = delete;
    LightSceneProxy& operator=(LightSceneProxy&&) = delete;
    virtual ~LightSceneProxy() noexcept;

    LightType GetLightType() const noexcept { return _lightType; }
    const Eigen::Matrix4f& GetLightToWorld() const noexcept { return _lightToWorld; }
    const Eigen::Matrix4f& GetWorldToLight() const noexcept { return _worldToLight; }
    const Eigen::Vector4f& GetPosition() const noexcept { return _position; }
    Eigen::Vector3f GetOrigin() const noexcept { return _position.head<3>(); }
    const Eigen::Vector3f& GetDirection() const noexcept { return _direction; }
    const Eigen::Vector3f& GetColor() const noexcept { return _color; }
    bool AffectsWorld() const noexcept { return _affectsWorld; }
    bool CastShadow() const noexcept { return _castShadow; }

    virtual float GetRadius() const noexcept { return std::numeric_limits<float>::max(); }
    virtual float GetSourceRadius() const noexcept { return 0.0f; }
    virtual bool IsInverseSquared() const noexcept { return true; }
    virtual bool IsLocalLight() const noexcept { return false; }
    virtual void GetLightRenderParameters(LightRenderParameters& out) const noexcept;

protected:
    void SetTransform(const Eigen::Matrix4f& lightToWorld, const Eigen::Vector4f& position) noexcept;
    void SetColor(const Eigen::Vector3f& color) noexcept { _color = color; }

private:
    LightType _lightType{LightType::Point};
    Eigen::Matrix4f _lightToWorld{Eigen::Matrix4f::Identity()};
    Eigen::Matrix4f _worldToLight{Eigen::Matrix4f::Identity()};
    Eigen::Vector4f _position{0.0f, 0.0f, 0.0f, 1.0f};
    Eigen::Vector3f _direction{Eigen::Vector3f::UnitZ()};
    Eigen::Vector3f _color{Eigen::Vector3f::Ones()};
    bool _affectsWorld{true};
    bool _castShadow{true};
};

}  // namespace radray
