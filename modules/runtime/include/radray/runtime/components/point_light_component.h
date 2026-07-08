#pragma once

#include <radray/runtime/components/light_component.h>

namespace radray {

class PointLightComponent : public LightComponent {
public:
    PointLightComponent() noexcept = default;
    ~PointLightComponent() noexcept override;

    RuntimeTypeId GetTypeId() const noexcept override;

    LightType GetLightType() const noexcept override { return LightType::Point; }
    unique_ptr<LightSceneProxy> CreateSceneProxy() const override;

    void SetAttenuationRadius(float radius) noexcept;
    float GetAttenuationRadius() const noexcept { return _attenuationRadius; }
    float GetInvAttenuationRadius() const noexcept;

    void SetUseInverseSquaredFalloff(bool value) noexcept;
    bool UseInverseSquaredFalloff() const noexcept { return _useInverseSquaredFalloff; }

    void SetLightFalloffExponent(float exponent) noexcept;
    float GetLightFalloffExponent() const noexcept { return _lightFalloffExponent; }

    void SetSourceRadius(float radius) noexcept;
    float GetSourceRadius() const noexcept { return _sourceRadius; }

    void SetSoftSourceRadius(float radius) noexcept;
    float GetSoftSourceRadius() const noexcept { return _softSourceRadius; }

    void SetSourceLength(float length) noexcept;
    float GetSourceLength() const noexcept { return _sourceLength; }

private:
    float _attenuationRadius{1000.0f};
    float _lightFalloffExponent{8.0f};
    float _sourceRadius{0.0f};
    float _softSourceRadius{0.0f};
    float _sourceLength{0.0f};
    bool _useInverseSquaredFalloff{true};
};

template <>
struct RuntimeTypeTrait<PointLightComponent> {
    static constexpr RuntimeTypeId value{0xd289eae9, 0xf0e6, 0x45cc, 0x9d, 0x55, 0xb8, 0xd1, 0x02, 0x2a, 0xa9, 0x6d};
    using Bases = std::tuple<LightComponent>;
};

}  // namespace radray
