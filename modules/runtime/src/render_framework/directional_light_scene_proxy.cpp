#include <radray/runtime/render_framework/directional_light_scene_proxy.h>

#include <algorithm>

#include <radray/runtime/components/directional_light_component.h>

namespace radray {

DirectionalLightSceneProxy::DirectionalLightSceneProxy(const DirectionalLightComponent& component)
    : LightSceneProxy(component),
      _cascadeCount(std::clamp<uint32_t>(component.GetCascadeCount(), 1u, 4u)),
      _shadowDistance(std::max(component.GetShadowDistance(), 0.0f)),
      _cascadeSplitLambda(std::clamp(component.GetCascadeSplitLambda(), 0.0f, 1.0f)),
      _shadowMapResolution(std::max<uint32_t>(component.GetShadowMapResolution(), 1u)),
      _shadowDepthBias(std::max(component.GetShadowDepthBias(), 0.0f)),
      _shadowNormalBias(std::max(component.GetShadowNormalBias(), 0.0f)),
      _shadowSoftMode(std::min<uint32_t>(component.GetShadowSoftMode(), 2u)) {
}

DirectionalLightSceneProxy::~DirectionalLightSceneProxy() noexcept = default;

}  // namespace radray
