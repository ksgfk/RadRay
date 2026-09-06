#pragma once

#include <radray/runtime/components/point_light_component.h>

namespace radray {

class SpotLightComponent : public PointLightComponent {
public:
    LightType GetLightType() const noexcept override { return LightType::Spot; }
    unique_ptr<LightSceneProxy> CreateSceneProxy() const override;
    /// Half-angles in radians, 0 <= inner < outer < pi / 2. Invalid input leaves the component unchanged.
    bool SetConeAngles(float inner, float outer) noexcept;
    float GetInnerConeAngle() const noexcept { return _inner; }
    float GetOuterConeAngle() const noexcept { return _outer; }

private:
    float _inner{0.34906585f}, _outer{0.52359878f};
};

template <>
struct RuntimeTypeTrait<SpotLightComponent> {
    static constexpr RuntimeTypeId value{0x7ef84126, 0x5e84, 0x465d, 0x82, 0xb6, 0x3a, 0x9c, 0x05, 0xf1, 0x27, 0xe4};
};

}  // namespace radray
