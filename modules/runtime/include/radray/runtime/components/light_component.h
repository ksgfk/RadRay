#pragma once

#include <radray/runtime/components/scene_component.h>
#include <radray/types.h>

namespace radray {

enum class LightType : uint8_t {
    Directional,
    Point,
    Spot,
    Rect,
};

class LightComponent : public SceneComponent {
public:
    LightComponent() noexcept = default;
    ~LightComponent() noexcept override;

    virtual LightType GetLightType() const noexcept { return LightType::Point; }
    virtual Eigen::Vector4f GetLightPosition() const noexcept;
    virtual Eigen::Vector3f GetLightDirection() const noexcept;

    void SetIntensity(float intensity) noexcept;
    float GetIntensity() const noexcept { return _intensity; }

    void SetLightColor(const Eigen::Vector3f& color) noexcept;
    const Eigen::Vector3f& GetLightColor() const noexcept { return _lightColor; }
    Eigen::Vector3f GetColoredLightBrightness() const noexcept { return _lightColor * _intensity; }

    void SetAffectsWorld(bool affectsWorld) noexcept;
    bool AffectsWorld() const noexcept { return _affectsWorld; }

    void SetCastShadow(bool castShadow) noexcept;
    bool CastShadow() const noexcept { return _castShadow; }

private:
    float _intensity{1.0f};
    Eigen::Vector3f _lightColor{Eigen::Vector3f::Ones()};
    bool _affectsWorld{true};
    bool _castShadow{true};
};

template <>
struct RuntimeTypeTrait<LightComponent> {
    static constexpr RuntimeTypeId value{0x37e0491c, 0x0429, 0x4b5d, 0xb1, 0x40, 0x72, 0xec, 0x91, 0x99, 0x56, 0x45};
};

}  // namespace radray
