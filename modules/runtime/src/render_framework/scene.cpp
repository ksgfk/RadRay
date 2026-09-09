#include <radray/runtime/render_framework/scene.h>

#include <algorithm>
#include <limits>

#include <radray/runtime/render_framework/light_scene_proxy.h>
#include <radray/runtime/render_framework/primitive_scene_proxy.h>

namespace radray {

constexpr uint32_t kInvalidPacked = std::numeric_limits<uint32_t>::max();

uint32_t Scene::SlotTable::Allocate() {
    if (!Free.empty()) {
        const uint32_t slot = Free.back();
        Free.pop_back();
        Packed[slot] = 0;
        return slot;
    }
    const uint32_t slot = static_cast<uint32_t>(Generation.size());
    Generation.push_back(1);
    Packed.push_back(kInvalidPacked);
    return slot;
}

void Scene::SlotTable::Release(uint32_t slot) noexcept {
    if (slot >= Generation.size()) return;
    if (Generation[slot] != std::numeric_limits<uint32_t>::max()) ++Generation[slot];
    Packed[slot] = kInvalidPacked;
    Free.push_back(slot);
}

Scene::~Scene() noexcept = default;

Nullable<PrimitiveSceneProxy*> Scene::AddPrimitive(unique_ptr<PrimitiveSceneProxy> proxy) {
    if (proxy == nullptr) return nullptr;
    PrimitiveSceneProxy* raw = proxy.get();
    const uint32_t slot = _primitiveSlots.Allocate();
    const SceneObjectId id{slot, _primitiveSlots.Generation[slot]};
    _primitiveSlots.Packed[slot] = static_cast<uint32_t>(_primitiveProxies.size());
    _primitiveProxies.push_back(std::move(proxy));
    _primitiveIds.push_back(id);
    _primitiveByPointer.emplace(raw, id);
    return raw;
}

void Scene::RemovePrimitive(PrimitiveSceneProxy* proxy) noexcept {
    if (proxy == nullptr) return;
    auto it = std::find_if(_primitiveProxies.begin(), _primitiveProxies.end(),
                           [proxy](const unique_ptr<PrimitiveSceneProxy>& candidate) {
                               return candidate.get() == proxy;
                           });
    if (it == _primitiveProxies.end()) return;
    const auto packed = static_cast<uint32_t>(it - _primitiveProxies.begin());
    const SceneObjectId id = _primitiveIds[packed];
    _primitiveByPointer.erase(proxy);
    _primitiveSlots.Release(id.Slot);
    _primitiveProxies.erase(it);
    _primitiveIds.erase(_primitiveIds.begin() + packed);
    for (size_t index = packed; index < _primitiveIds.size(); ++index)
        _primitiveSlots.Packed[_primitiveIds[index].Slot] = static_cast<uint32_t>(index);
}

Nullable<LightSceneProxy*> Scene::AddLight(unique_ptr<LightSceneProxy> proxy) {
    if (proxy == nullptr) return nullptr;
    LightSceneProxy* raw = proxy.get();
    const uint32_t slot = _lightSlots.Allocate();
    const SceneObjectId id{slot, _lightSlots.Generation[slot]};
    _lightSlots.Packed[slot] = static_cast<uint32_t>(_lightProxies.size());
    _lightProxies.push_back(std::move(proxy));
    _lightIds.push_back(id);
    _lightByPointer.emplace(raw, id);
    return raw;
}

void Scene::RemoveLight(LightSceneProxy* proxy) noexcept {
    if (proxy == nullptr) return;
    auto it = std::find_if(_lightProxies.begin(), _lightProxies.end(),
                           [proxy](const unique_ptr<LightSceneProxy>& candidate) {
                               return candidate.get() == proxy;
                           });
    if (it == _lightProxies.end()) return;
    const auto packed = static_cast<uint32_t>(it - _lightProxies.begin());
    const SceneObjectId id = _lightIds[packed];
    _lightByPointer.erase(proxy);
    _lightSlots.Release(id.Slot);
    _lightProxies.erase(it);
    _lightIds.erase(_lightIds.begin() + packed);
    for (size_t index = packed; index < _lightIds.size(); ++index)
        _lightSlots.Packed[_lightIds[index].Slot] = static_cast<uint32_t>(index);
}

SceneObjectId Scene::GetPrimitiveId(const PrimitiveSceneProxy* proxy) const noexcept {
    const auto found = _primitiveByPointer.find(proxy);
    return found == _primitiveByPointer.end() ? SceneObjectId{} : found->second;
}

Nullable<PrimitiveSceneProxy*> Scene::FindPrimitive(SceneObjectId id) const noexcept {
    if (!id.IsValid() || id.Slot >= _primitiveSlots.Generation.size() ||
        _primitiveSlots.Generation[id.Slot] != id.Generation || _primitiveSlots.Packed[id.Slot] == kInvalidPacked)
        return nullptr;
    return _primitiveProxies[_primitiveSlots.Packed[id.Slot]].get();
}

SceneObjectId Scene::GetLightId(const LightSceneProxy* proxy) const noexcept {
    const auto found = _lightByPointer.find(proxy);
    return found == _lightByPointer.end() ? SceneObjectId{} : found->second;
}

Nullable<LightSceneProxy*> Scene::FindLight(SceneObjectId id) const noexcept {
    if (!id.IsValid() || id.Slot >= _lightSlots.Generation.size() ||
        _lightSlots.Generation[id.Slot] != id.Generation || _lightSlots.Packed[id.Slot] == kInvalidPacked)
        return nullptr;
    return _lightProxies[_lightSlots.Packed[id.Slot]].get();
}

}  // namespace radray
