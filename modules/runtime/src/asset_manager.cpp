#include <radray/runtime/asset_manager.h>

#include <algorithm>

#include <radray/logger.h>

namespace radray {

AssetManager::~AssetManager() noexcept {
    // 强制回收所有 slot,无论引用计数。析构期不再有人持有 AssetRef。
    for (auto& slot : _slots) {
        if (slot && slot->State != AssetState::Free && slot->Object) {
            slot->Object->OnUnload();
            slot->Object.reset();
            slot->State = AssetState::Free;
        }
    }
}

void AssetManager::AddRef(AssetHandle handle) noexcept {
    // 强引用钉住 slot,地址与 generation 必然有效,无需加锁。
    RADRAY_ASSERT(handle.IsValid() && handle.Index < _slots.size());
    Slot* slot = _slots[handle.Index].get();
    RADRAY_ASSERT(slot != nullptr && slot->Generation == handle.Generation);
    slot->StrongRefs.fetch_add(1, std::memory_order_relaxed);
}

void AssetManager::Release(AssetHandle handle) noexcept {
    RADRAY_ASSERT(handle.IsValid() && handle.Index < _slots.size());
    Slot* slot = _slots[handle.Index].get();
    RADRAY_ASSERT(slot != nullptr && slot->Generation == handle.Generation);
    int32_t prev = slot->StrongRefs.fetch_sub(1, std::memory_order_acq_rel);
    RADRAY_ASSERT(prev >= 1);
    if (prev == 1) {
        // 归零:标记 PendingRelease 并入队,真正销毁延迟到 CollectGarbage。
        // 复合操作(改状态 + 入队)需加锁,与 Lock/CollectGarbage 互斥。
        std::scoped_lock lk(_mutex);
        // 再次确认仍为 0,期间可能已被 Lock 复活。
        if (slot->StrongRefs.load(std::memory_order_acquire) == 0 &&
            slot->State == AssetState::Loaded) {
            slot->State = AssetState::PendingRelease;
            _pending.push_back(handle.Index);
        }
    }
}

Nullable<Asset*> AssetManager::Resolve(AssetHandle handle) const noexcept {
    // 调用方持有强引用,slot 不会被回收,可无锁读取。
    if (!handle.IsValid() || handle.Index >= _slots.size()) {
        return nullptr;
    }
    const Slot* slot = _slots[handle.Index].get();
    if (slot == nullptr || slot->Generation != handle.Generation) {
        return nullptr;
    }
    if (slot->State == AssetState::Free || slot->State == AssetState::Failed) {
        return nullptr;
    }
    return slot->Object.get();
}

AssetTypeId AssetManager::ResolveTypeId(AssetHandle handle) const noexcept {
    if (!handle.IsValid() || handle.Index >= _slots.size()) {
        return AssetTypeId::Invalid();
    }
    const Slot* slot = _slots[handle.Index].get();
    if (slot == nullptr || slot->Generation != handle.Generation) {
        return AssetTypeId::Invalid();
    }
    if (slot->State == AssetState::Free || slot->State == AssetState::Failed) {
        return AssetTypeId::Invalid();
    }
    return slot->TypeId;
}

AssetHandle AssetManager::AcquireExistingLocked(const AssetId& id) noexcept {
    auto it = _idIndex.find(id);
    if (it == _idIndex.end()) {
        return AssetHandle::Invalid();
    }
    const uint32_t index = it->second;
    Slot* slot = _slots[index].get();
    RADRAY_ASSERT(slot != nullptr);
    if (slot->State == AssetState::Free) {
        // 不应出现:Free slot 不该残留在 id 索引里。
        return AssetHandle::Invalid();
    }
    // +1 计数;若处于 PendingRelease 则复活。
    slot->StrongRefs.fetch_add(1, std::memory_order_relaxed);
    if (slot->State == AssetState::PendingRelease) {
        slot->State = AssetState::Loaded;
        auto pit = std::find(_pending.begin(), _pending.end(), index);
        if (pit != _pending.end()) {
            _pending.erase(pit);
        }
    }
    return AssetHandle{index, slot->Generation};
}

AssetHandle AssetManager::InsertLocked(unique_ptr<Asset> object, const AssetId& id, AssetTypeId typeId) noexcept {
    uint32_t index;
    if (!_freeSlots.empty()) {
        index = _freeSlots.back();
        _freeSlots.pop_back();
    } else {
        index = static_cast<uint32_t>(_slots.size());
        _slots.push_back(make_unique<Slot>());
    }
    Slot* slot = _slots[index].get();
    slot->Object = std::move(object);
    slot->TypeId = typeId;
    slot->State = AssetState::Loaded;
    slot->StrongRefs.store(1, std::memory_order_relaxed);
    _idIndex.emplace(id, index);
    ++_aliveCount;
    return AssetHandle{index, slot->Generation};
}

AssetHandle AssetManager::LockLocked(AssetHandle handle) noexcept {
    if (!handle.IsValid() || handle.Index >= _slots.size()) {
        return AssetHandle::Invalid();
    }
    Slot* slot = _slots[handle.Index].get();
    if (slot == nullptr || slot->Generation != handle.Generation) {
        return AssetHandle::Invalid();
    }
    if (slot->State == AssetState::Free || slot->State == AssetState::Failed) {
        return AssetHandle::Invalid();
    }
    slot->StrongRefs.fetch_add(1, std::memory_order_relaxed);
    if (slot->State == AssetState::PendingRelease) {
        slot->State = AssetState::Loaded;
        auto pit = std::find(_pending.begin(), _pending.end(), handle.Index);
        if (pit != _pending.end()) {
            _pending.erase(pit);
        }
    }
    return AssetHandle{handle.Index, slot->Generation};
}

void AssetManager::CollectGarbage() {
    // 收集本轮要销毁的 slot,Asset 析构放到锁外执行(OnUnload 可能耗时)。
    vector<unique_ptr<Asset>> toDestroy;
    {
        std::scoped_lock lk(_mutex);
        for (uint32_t index : _pending) {
            Slot* slot = _slots[index].get();
            if (slot == nullptr || slot->State != AssetState::PendingRelease) {
                continue;  // 已被处理或复活
            }
            // 复活复检:期间可能 Lock 把计数加回。
            if (slot->StrongRefs.load(std::memory_order_acquire) != 0) {
                // 已复活但状态未复位(防御性处理):恢复 Loaded,
                // 否则后续 Release 不会重新入队导致泄漏。
                slot->State = AssetState::Loaded;
                continue;
            }
            // 确认销毁:清 id 索引、generation++、slot 置空待复用。
            if (!slot->Object) {
                slot->State = AssetState::Free;
                continue;
            }
            _idIndex.erase(slot->Object->GetAssetId());
            toDestroy.push_back(std::move(slot->Object));
            ++slot->Generation;
            slot->State = AssetState::Free;
            slot->StrongRefs.store(0, std::memory_order_relaxed);
            _freeSlots.push_back(index);
            RADRAY_ASSERT(_aliveCount > 0);
            --_aliveCount;
        }
        _pending.clear();
    }
    // 锁外销毁:触发 OnUnload + 析构。
    for (auto& object : toDestroy) {
        if (object) {
            object->OnUnload();
        }
    }
}

uint32_t AssetManager::GetAssetCount() const noexcept {
    std::scoped_lock lk(_mutex);
    return _aliveCount;
}

}  // namespace radray
