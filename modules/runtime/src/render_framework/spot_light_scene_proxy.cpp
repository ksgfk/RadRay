#include <radray/runtime/render_framework/spot_light_scene_proxy.h>

#include <cmath>
#include <radray/runtime/components/spot_light_component.h>

namespace radray {

SpotLightSceneProxy::SpotLightSceneProxy(const SpotLightComponent& component) : PointLightSceneProxy(component) {
    const float outer = std::cos(component.GetOuterConeAngle());
    _spotAngles = {outer, 1.0f / (std::cos(component.GetInnerConeAngle()) - outer)};
}

void SpotLightSceneProxy::GetLightRenderParameters(LightRenderParameters& out) const noexcept {
    PointLightSceneProxy::GetLightRenderParameters(out);
    out.SpotAngles = _spotAngles;
    out.Direction = GetDirection().normalized();
}

}  // namespace radray
