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

void DirectionalLightComponent::SetCascadeSplitMode(CascadeSplitMode mode) noexcept {
    _cascadeSplitMode = mode;
    MarkRenderStateDirty();
}

void DirectionalLightComponent::SetCascadeSplitLambda(float lambda) noexcept {
    _cascadeSplitLambda = std::clamp(lambda, 0.0f, 1.0f);
    MarkRenderStateDirty();
}

void DirectionalLightComponent::SetCascadeSplitRatios(const std::array<float, 3>& ratios) noexcept {
    // 各比例 clamp 到 [0,1] 并强制单调不减, 保证级联边界有序。
    float prev = 0.0f;
    for (size_t i = 0; i < _cascadeSplitRatios.size(); ++i) {
        const float v = std::clamp(ratios[i], prev, 1.0f);
        _cascadeSplitRatios[i] = v;
        prev = v;
    }
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
