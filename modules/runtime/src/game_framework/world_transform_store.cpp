#include <radray/runtime/game_framework/world_transform_store.h>

#include <radray/logger.h>
#include <radray/runtime/components/scene_component.h>

namespace radray {

WorldTransformId WorldTransformStore::Allocate(SceneComponent* component, const LocalTransform& local) {
    uint32_t index;
    if (_free.empty()) {
        if (_values.size() == std::numeric_limits<uint32_t>::max()) RADRAY_ABORT("Too many World transforms");
        index = static_cast<uint32_t>(_values.size());
        const bool relocating = _values.size() == _values.capacity();
        _values.emplace_back();
        if (relocating) {
            for (size_t i = 0; i < _slots.size(); ++i) {
                if (auto owner = _slots[i].Component) owner->_local = &_values[i].Local;
            }
        }
        _slots.emplace_back();
        _dirtyBits.resize((_values.size() + 63) / 64, 0);
    } else {
        index = _free.back();
        _free.pop_back();
    }
    _values[index] = {{}, local};
    _slots[index].Component = component;
    return {index, _slots[index].Generation};
}

void WorldTransformStore::Release(WorldTransformId id) {
    if (!Find(id)) RADRAY_ABORT("Invalid World transform");
    auto& slot = _slots[id.Index];
    if (slot.Generation == std::numeric_limits<uint32_t>::max()) RADRAY_ABORT("World transform generation exhausted");
    ++slot.Generation;
    slot.Component = nullptr;
    _values[id.Index].Id = {};
    _free.push_back(id.Index);
}

Nullable<SceneComponent*> WorldTransformStore::Find(WorldTransformId id) const noexcept {
    if (id.Index >= _slots.size() || _slots[id.Index].Generation != id.Generation) return nullptr;
    return _slots[id.Index].Component;
}

void WorldTransformStore::ClearChanges() noexcept {
    for (const auto block : _dirtyBlocks) _dirtyBits[block] = 0;
    _dirtyBlocks.clear();
    _dirtyRows.clear();
}

}  // namespace radray
