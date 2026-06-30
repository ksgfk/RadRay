#include <radray/runtime/render_framework/light_scene_proxy.h>

#include <radray/runtime/components/light_component.h>

namespace radray {

LightSceneProxy::LightSceneProxy(const LightComponent& component)
    : _lightType(component.GetLightType()),
      _color(component.GetColoredLightBrightness()),
      _affectsWorld(component.AffectsWorld()),
      _castShadow(component.CastShadow()) {
    SetTransform(component.GetWorldMatrix(), component.GetLightPosition());
    _direction = component.GetLightDirection();
}

LightSceneProxy::~LightSceneProxy() noexcept = default;

void LightSceneProxy::GetLightRenderParameters(LightRenderParameters& out) const noexcept {
    out.WorldPosition = GetOrigin();
    out.Color = GetColor();
    out.Direction = GetDirection();
    out.SpecularScale = 1.0f;
    out.DiffuseScale = 1.0f;
    out.Tangent = Eigen::Vector3f{_worldToLight(0, 2), _worldToLight(1, 2), _worldToLight(2, 2)};
}

void LightSceneProxy::SetTransform(const Eigen::Matrix4f& lightToWorld, const Eigen::Vector4f& position) noexcept {
    _lightToWorld = lightToWorld;
    _worldToLight = _lightToWorld.inverse();
    _position = position;
}

}  // namespace radray
