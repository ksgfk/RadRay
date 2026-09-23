#pragma once

#include <radray/nullable.h>
#include <radray/runtime/render_scene/scene_transform.h>

namespace radray {

class SceneComponent;

/// Scoped to one World; retained handles are invalidated by component unregistration.
struct WorldTransformId {
    uint32_t Index{std::numeric_limits<uint32_t>::max()};
    uint32_t Generation{0};
    bool IsValid() const noexcept { return Generation != 0; }
    bool operator==(const WorldTransformId&) const noexcept = default;
};

struct WorldTransformUpdate {
    WorldTransformId Id;
    LocalTransform Local;
};

/// GT-owned packed local values. RT receives independent value packets, never this storage.
class WorldTransformStore {
private:
    friend class World;
    friend class SceneComponent;
    friend class WorldRenderBridge;

    struct Slot {
        Nullable<SceneComponent*> Component{nullptr};
        uint32_t Generation{1};
    };

    WorldTransformId Allocate(SceneComponent* component, const LocalTransform& local);
    void Release(WorldTransformId id);
    Nullable<SceneComponent*> Find(WorldTransformId id) const noexcept;
    void MarkChanged(uint32_t index) {
        if (!_values[index].Id.IsValid()) return;
        const uint32_t block = index / 64;
        auto& bits = _dirtyBits[block];
        const uint64_t bit = uint64_t{1} << (index % 64);
        if ((bits & bit) != 0) return;
        if (bits == 0) _dirtyBlocks.push_back(block);
        bits |= bit;
        _dirtyRows.push_back(index);
    }
    void ClearChanges() noexcept;
    LocalTransform& GetLocal(uint32_t index) noexcept { return _values[index].Local; }
    const LocalTransform& GetLocal(uint32_t index) const noexcept { return _values[index].Local; }

    vector<LocalTransformUpdate> _values;
    vector<Slot> _slots;
    vector<uint32_t> _free;
    vector<uint64_t> _dirtyBits;
    vector<uint32_t> _dirtyBlocks;
    vector<uint32_t> _dirtyRows;
};

}  // namespace radray
