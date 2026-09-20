#include <radray/runtime/components/spot_light_component.h>

#include <cmath>
#include <numbers>

namespace radray {

bool SpotLightComponent::SetConeAngles(float inner, float outer) noexcept {
    CheckCanModify();
    if (!std::isfinite(inner) || !std::isfinite(outer) || inner < 0 || inner >= outer || outer >= std::numbers::pi_v<float> / 2 ||
        std::cos(inner) <= std::cos(outer)) return false;
    if (_inner == inner && _outer == outer) return true;
    _inner = inner;
    _outer = outer;
    MarkRenderDynamicDataDirty();
    return true;
}

LightData SpotLightComponent::CaptureLightState() const noexcept {
    auto result = PointLightComponent::CaptureLightState();
    auto& light = std::get<SpotLightData>(result);
    light.InnerConeAngle = _inner;
    light.OuterConeAngle = _outer;
    return result;
}

}  // namespace radray
