#pragma once

#include <radray/runtime/render_framework/light_scene_proxy.h>

namespace radray {

class PointLightComponent;

class PointLightSceneProxy final : public LightSceneProxy {
public:
    explicit PointLightSceneProxy(const PointLightComponent& component);
    ~PointLightSceneProxy() noexcept override;

    float GetRadius() const noexcept override { return _radius; }
    float GetInvRadius() const noexcept { return _invRadius; }
    float GetFalloffExponent() const noexcept { return _falloffExponent; }
    float GetSourceRadius() const noexcept override { return _sourceRadius; }
    float GetSoftSourceRadius() const noexcept { return _softSourceRadius; }
    float GetSourceLength() const noexcept { return _sourceLength; }
    bool IsInverseSquared() const noexcept override { return _inverseSquared; }
    bool IsLocalLight() const noexcept override { return true; }

    void GetLightRenderParameters(LightRenderParameters& out) const noexcept override;

private:
    float _radius{1000.0f};
    float _invRadius{0.001f};
    float _falloffExponent{8.0f};
    float _sourceRadius{0.0f};
    float _softSourceRadius{0.0f};
    float _sourceLength{0.0f};
    bool _inverseSquared{true};
};

}  // namespace radray
