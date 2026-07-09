#include <radray/runtime/components/directional_light_component.h>

#include <algorithm>

#include <radray/runtime/render_framework/directional_light_scene_proxy.h>

namespace radray {

DirectionalLightComponent::~DirectionalLightComponent() noexcept = default;

RuntimeTypeId DirectionalLightComponent::GetTypeId() const noexcept {
    return runtime_type_id_v<DirectionalLightComponent>;
}

unique_ptr<LightSceneProxy> DirectionalLightComponent::CreateSceneProxy() const {
    return make_unique<DirectionalLightSceneProxy>(*this);
}

void DirectionalLightComponent::SetCascadeCount(uint32_t count) noexcept {
    _cascadeCount = std::clamp<uint32_t>(count, 1u, 4u);
    MarkRenderStateDirty();
}

void DirectionalLightComponent::SetShadowDistance(float distance) noexcept {
    _shadowDistance = std::max(distance, 0.0f);
    MarkRenderStateDirty();
}

void DirectionalLightComponent::SetCascadeSplitLambda(float lambda) noexcept {
    _cascadeSplitLambda = std::clamp(lambda, 0.0f, 1.0f);
    MarkRenderStateDirty();
}

void DirectionalLightComponent::SetShadowMapResolution(uint32_t resolution) noexcept {
    _shadowMapResolution = std::max<uint32_t>(resolution, 1u);
    MarkRenderStateDirty();
}

void DirectionalLightComponent::SetShadowDepthBias(float bias) noexcept {
    _shadowDepthBias = std::max(bias, 0.0f);
    MarkRenderStateDirty();
}

void DirectionalLightComponent::SetShadowNormalBias(float bias) noexcept {
    _shadowNormalBias = std::max(bias, 0.0f);
    MarkRenderStateDirty();
}

void DirectionalLightComponent::SetShadowSoftMode(uint32_t mode) noexcept {
    _shadowSoftMode = std::min<uint32_t>(mode, 2u);
    MarkRenderStateDirty();
}

}  // namespace radray
