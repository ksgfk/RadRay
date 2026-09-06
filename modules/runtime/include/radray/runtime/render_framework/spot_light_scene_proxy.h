#pragma once

#include <radray/runtime/render_framework/point_light_scene_proxy.h>

namespace radray {

class SpotLightComponent;

class SpotLightSceneProxy final : public PointLightSceneProxy {
public:
    explicit SpotLightSceneProxy(const SpotLightComponent& component);
    void GetLightRenderParameters(LightRenderParameters& out) const noexcept override;

private:
    Eigen::Vector2f _spotAngles;
};

}  // namespace radray
