#include <radray/runtime/render_scene/light_scene_data.h>

#include <algorithm>
#include <limits>
#include <type_traits>
#include <utility>

#include <radray/logger.h>

namespace radray {
namespace {

constexpr size_t kNoRow = std::numeric_limits<size_t>::max();

/// Scans the id column only; payload columns are never touched by a lookup.
template <class T>
size_t FindRow(const LightTable<T>& table, LightId id) noexcept {
    const auto found = std::find(table.Ids.begin(), table.Ids.end(), id);
    return found == table.Ids.end() ? kNoRow : static_cast<size_t>(found - table.Ids.begin());
}

template <class T>
Nullable<const T*> FindLight(const LightTable<T>& table, LightId id) noexcept {
    if (!id.IsValid()) return nullptr;
    const auto row = FindRow(table, id);
    return row == kNoRow ? nullptr : &table.Data[row];
}

template <class T>
bool RemoveLight(LightTable<T>& table, LightId id) noexcept {
    const auto row = FindRow(table, id);
    if (row == kNoRow) return false;
    const auto last = table.Ids.size() - 1;
    if (row != last) {
        table.Ids[row] = table.Ids[last];
        table.Data[row] = std::move(table.Data[last]);
    }
    table.Ids.pop_back();
    table.Data.pop_back();
    return true;
}

template <class T>
void AssignLights(LightTable<T>& target, const LightTable<T>& source) noexcept {
    if (source.Ids.size() != source.Data.size()) RADRAY_ABORT("Light table columns must match");
    for (LightId id : source.Ids) {
        if (!id.IsValid()) RADRAY_ABORT("Invalid light identity");
    }
    target.Ids.assign(source.Ids.begin(), source.Ids.end());
    target.Data.assign(source.Data.begin(), source.Data.end());
}

template <class T>
void ClearLights(LightTable<T>& table) noexcept {
    table.Ids.clear();
    table.Data.clear();
}

}  // namespace

void LightSceneData::Clear() noexcept {
    ClearLights(DirectionalLights);
    ClearLights(PointLights);
    ClearLights(SpotLights);
    ClearLights(RectLights);
}

void LightSceneData::Assign(const LightSceneData& lights) noexcept {
    if (this == &lights) return;
    AssignLights(DirectionalLights, lights.DirectionalLights);
    AssignLights(PointLights, lights.PointLights);
    AssignLights(SpotLights, lights.SpotLights);
    AssignLights(RectLights, lights.RectLights);
}

void LightSceneData::Set(LightId id, const LightData& light) noexcept {
    if (!id.IsValid()) RADRAY_ABORT("Invalid light identity");
    std::visit([this, id](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        auto& table = GetTable<T>();
        const auto row = FindRow(table, id);
        if (row != kNoRow) {
            table.Data[row] = value;
        } else {
            Remove(id);
            table.Ids.push_back(id);
            table.Data.push_back(value);
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
