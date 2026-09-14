#include <radray/runtime/components/spot_light_component.h>

#include <cmath>
#include <numbers>

namespace radray {

bool SpotLightComponent::SetConeAngles(float inner, float outer) noexcept {
    if (!std::isfinite(inner) || !std::isfinite(outer) || inner < 0 || inner >= outer || outer >= std::numbers::pi_v<float> / 2 ||
        std::cos(inner) <= std::cos(outer)) return false;
    _inner = inner;
    _outer = outer;
    return true;
}


}  // namespace radray
