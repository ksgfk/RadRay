#include <radray/runtime/render_framework/point_light_scene_proxy.h>

#include <algorithm>

#include <radray/runtime/components/point_light_component.h>

namespace radray {

PointLightSceneProxy::PointLightSceneProxy(const PointLightComponent& component)
    : LightSceneProxy(component),
      _radius(std::max(component.GetAttenuationRadius(), 0.0f)),
      _invRadius(component.GetInvAttenuationRadius()),
      _falloffExponent(component.GetLightFalloffExponent()),
      _sourceRadius(component.GetSourceRadius()),
      _softSourceRadius(component.GetSoftSourceRadius()),
      _sourceLength(component.GetSourceLength()),
      _inverseSquared(component.UseInverseSquaredFalloff()),
      _shadowDepthBias(std::max(component.GetShadowDepthBias(), 0.0f)),
      _shadowNormalBias(std::max(component.GetShadowNormalBias(), 0.0f)) {
}

PointLightSceneProxy::~PointLightSceneProxy() noexcept = default;

void PointLightSceneProxy::GetLightRenderParameters(LightRenderParameters& out) const noexcept {
    LightSceneProxy::GetLightRenderParameters(out);
    out.InvRadius = _invRadius;
    out.FalloffExponent = _inverseSquared ? 0.0f : _falloffExponent;
    out.SourceRadius = _sourceRadius;
    out.SoftSourceRadius = _softSourceRadius;
    out.SourceLength = _sourceLength;
    out.SpotAngles = Eigen::Vector2f{-2.0f, 1.0f};
}

}  // namespace radray
