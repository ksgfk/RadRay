#include <radray/runtime/render_scene/light_scene_data.h>

#include <algorithm>
#include <type_traits>
#include <radray/logger.h>

namespace radray {
namespace {

template <class T>
Nullable<const T*> FindLight(const vector<T>& lights, LightId id) noexcept {
    if (!id.IsValid()) return nullptr;
    for (const auto& light : lights) {
        if (light.Common.Id == id) return &light;
    }
    return nullptr;
}

template <class T>
bool RemoveLight(vector<T>& lights, LightId id) noexcept {
    const auto found = std::find_if(lights.begin(), lights.end(), [id](const T& light) { return light.Common.Id == id; });
    if (found == lights.end()) return false;
    if (found != lights.end() - 1) *found = std::move(lights.back());
    lights.pop_back();
    return true;
}

template <class T>
void AssignLights(vector<T>& target, const vector<T>& source) noexcept {
    for (const auto& light : source) {
        if (!light.Common.Id.IsValid()) RADRAY_ABORT("Invalid light identity");
    }
    target.assign(source.begin(), source.end());
}

}  // namespace

void LightSceneData::Clear() noexcept {
    DirectionalLights.clear();
    PointLights.clear();
    SpotLights.clear();
    RectLights.clear();
}

void LightSceneData::Assign(const LightSceneData& lights) noexcept {
    if (this == &lights) return;
    AssignLights(DirectionalLights, lights.DirectionalLights);
    AssignLights(PointLights, lights.PointLights);
    AssignLights(SpotLights, lights.SpotLights);
    AssignLights(RectLights, lights.RectLights);
}

void LightSceneData::Set(const LightData& light) noexcept {
    std::visit([this](const auto& value) {
        const auto id = value.Common.Id;
        if (!id.IsValid()) RADRAY_ABORT("Invalid light identity");
        using T = std::decay_t<decltype(value)>;
        auto& lights = [this]() -> vector<T>& {
            if constexpr (std::is_same_v<T, DirectionalLightData>)
                return DirectionalLights;
            else if constexpr (std::is_same_v<T, PointLightData>)
                return PointLights;
            else if constexpr (std::is_same_v<T, SpotLightData>)
                return SpotLights;
            else
                return RectLights;
        }();
        const auto found = std::find_if(lights.begin(), lights.end(), [id](const T& item) { return item.Common.Id == id; });
        if (found != lights.end()) {
            *found = value;
        } else {
            Remove(id);
            lights.push_back(value);
        }
    },
               light);
}

bool LightSceneData::Remove(LightId id) noexcept {
    return RemoveLight(DirectionalLights, id) || RemoveLight(PointLights, id) || RemoveLight(SpotLights, id) || RemoveLight(RectLights, id);
}

Nullable<const LightCommonData*> LightSceneData::GetLight(LightId id) const noexcept {
    if (const auto light = GetDirectionalLight(id)) return &light->Common;
    if (const auto light = GetPointLight(id)) return &light->Common;
    if (const auto light = GetSpotLight(id)) return &light->Common;
    if (const auto light = GetRectLight(id)) return &light->Common;
    return nullptr;
}

Nullable<const DirectionalLightData*> LightSceneData::GetDirectionalLight(LightId id) const noexcept { return FindLight(DirectionalLights, id); }
Nullable<const PointLightData*> LightSceneData::GetPointLight(LightId id) const noexcept { return FindLight(PointLights, id); }
Nullable<const SpotLightData*> LightSceneData::GetSpotLight(LightId id) const noexcept { return FindLight(SpotLights, id); }
Nullable<const RectLightData*> LightSceneData::GetRectLight(LightId id) const noexcept { return FindLight(RectLights, id); }

}  // namespace radray
