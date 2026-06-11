#include <radray/runtime/asset_manager.h>

#include <algorithm>

#include <radray/logger.h>

namespace radray {

AssetRefAny::AssetRefAny() noexcept = default;

AssetRefAny::AssetRefAny(std::nullptr_t) noexcept {}

AssetRefAny::AssetRefAny(const AssetRefAny& other) noexcept
    : _manager(other._manager), _handle(other._handle) {
    if (_manager != nullptr && _handle.IsValid()) {
        _manager->AddRef(_handle);
    }
}

AssetRefAny::AssetRefAny(AssetRefAny&& other) noexcept
    : _manager(other._manager), _handle(other._handle) {
    other._manager = nullptr;
    other._handle = AssetHandle::Invalid();
}

AssetRefAny& AssetRefAny::operator=(const AssetRefAny& other) noexcept {
    if (this == &other) {
        return *this;
    }
    if (other._manager != nullptr && other._handle.IsValid()) {
        other._manager->AddRef(other._handle);
    }
    Reset();
    _manager = other._manager;
    _handle = other._handle;
    return *this;
}

AssetRefAny& AssetRefAny::operator=(AssetRefAny&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    Reset();
    _manager = other._manager;
    _handle = other._handle;
    other._manager = nullptr;
    other._handle = AssetHandle::Invalid();
    return *this;
}

AssetRefAny::~AssetRefAny() noexcept {
    Reset();
}

Asset* AssetRefAny::Get() const noexcept {
    if (_manager == nullptr || !_handle.IsValid()) {
        return nullptr;
    }
    return _manager->Resolve(_handle).Get();
}

Asset* AssetRefAny::operator->() const noexcept {
    return Get();
}

Asset& AssetRefAny::operator*() const noexcept {
    return *Get();
}

bool AssetRefAny::IsValid() const noexcept {
    return Get() != nullptr;
}

AssetRefAny::operator bool() const noexcept {
    return IsValid();
}

AssetHandle AssetRefAny::GetHandle() const noexcept {
    return _handle;
}

AssetTypeId AssetRefAny::GetTypeId() const noexcept {
    if (_manager == nullptr || !_handle.IsValid()) {
        return Guid::Empty();
    }
    return _manager->ResolveTypeId(_handle);
}

void AssetRefAny::Reset() noexcept {
    if (_manager != nullptr && _handle.IsValid()) {
        _manager->Release(_handle);
    }
    _manager = nullptr;
    _handle = AssetHandle::Invalid();
}

AssetRefAny::AssetRefAny(AssetManager* manager, AssetHandle handle) noexcept
    : _manager(manager), _handle(handle) {}

AssetManager::~AssetManager() noexcept {
    // 强制回收所有 slot,无论引用计数。析构期不再有人持有 AssetRef。
    for (auto& slot : _slots.Values()) {
        if (slot && slot->State != AssetState::Free && slot->Object) {
            slot->Object->OnUnload();
            slot->Object.reset();
            slot->State = AssetState::Free;
        }
    }
}

AssetRefAny AssetManager::LoadAny(const AssetId& id, unique_ptr<Asset> object) {
    if (!object) {
        return {};
    }

    AssetHandle handle;
    {
        std::scoped_lock lk(_mutex);
        if (_idIndex.find(id) != _idIndex.end()) {
            return {};
        }

        handle = _slots.Emplace(make_unique<Slot>());
        Slot* slot = _slots.Get(handle).get();
        RADRAY_ASSERT(slot != nullptr && slot->State == AssetState::Free);
        slot->Object = std::move(object);
        slot->State = AssetState::Loading;
        slot->StrongRefs.store(1, std::memory_order_relaxed);
        _idIndex.emplace(id, handle);
    }

    // 资产已在构造函数完成初始化(不再有二段式 OnLoad)。
    // 这里仅补充 AssetId 并置为 Loaded。
    {
        Slot* slot = _slots.Get(handle).get();
        RADRAY_ASSERT(slot != nullptr && slot->Object);
        slot->Object->_id = id;
    }

    {
        std::scoped_lock lk(_mutex);
        Slot* slot = _slots.Get(handle).get();
        RADRAY_ASSERT(slot != nullptr && slot->State == AssetState::Loading && slot->Object);
        slot->State = AssetState::Loaded;
    }
    return AssetRefAny{this, handle};
}

void AssetManager::AddRef(AssetHandle handle) noexcept {
    Slot* slot = _slots.Get(handle).get();
    RADRAY_ASSERT(slot != nullptr);
    slot->StrongRefs.fetch_add(1, std::memory_order_relaxed);
}

void AssetManager::Release(AssetHandle handle) noexcept {
    Slot* slot = _slots.Get(handle).get();
    RADRAY_ASSERT(slot != nullptr);
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
            _pending.push_back(handle);
        }
    }
}

Nullable<Asset*> AssetManager::Resolve(AssetHandle handle) const noexcept {
    // 调用方持有强引用,slot 不会被回收,可无锁读取。
    const unique_ptr<Slot>* slotPtr = _slots.TryGet(handle);
    if (slotPtr == nullptr) {
        return nullptr;
    }
    const Slot* slot = slotPtr->get();
    if (slot->State == AssetState::Free || slot->State == AssetState::Failed) {
        return nullptr;
    }
    return slot->Object.get();
}

AssetTypeId AssetManager::ResolveTypeId(AssetHandle handle) const noexcept {
    const unique_ptr<Slot>* slotPtr = _slots.TryGet(handle);
    if (slotPtr == nullptr) {
        return Guid::Empty();
    }
    const Slot* slot = slotPtr->get();
    if (slot->State == AssetState::Free || slot->State == AssetState::Failed) {
        return Guid::Empty();
    }
    if (!slot->Object) {
        return Guid::Empty();
    }
    return slot->Object->GetTypeId();
}

AssetHandle AssetManager::AcquireExistingLocked(const AssetId& id) noexcept {
    auto it = _idIndex.find(id);
    if (it == _idIndex.end()) {
        return AssetHandle::Invalid();
    }
    const AssetHandle handle = it->second;
    unique_ptr<Slot>* slotPtr = _slots.TryGet(handle);
    if (slotPtr == nullptr) {
        _idIndex.erase(it);
        return AssetHandle::Invalid();
    }
    Slot* slot = slotPtr->get();
    RADRAY_ASSERT(slot != nullptr);
    if (slot->State == AssetState::Free) {
        // 不应出现:Free slot 不该残留在 id 索引里。
        return AssetHandle::Invalid();
    }
    if (slot->State == AssetState::Loading || slot->State == AssetState::Failed || !slot->Object) {
        return AssetHandle::Invalid();
    }
    // +1 计数;若处于 PendingRelease 则复活。
    slot->StrongRefs.fetch_add(1, std::memory_order_relaxed);
    if (slot->State == AssetState::PendingRelease) {
        slot->State = AssetState::Loaded;
        auto pit = std::find(_pending.begin(), _pending.end(), handle);
        if (pit != _pending.end()) {
            _pending.erase(pit);
        }
    }
    return handle;
}

AssetRefAny AssetManager::LockAny(AssetHandle handle) noexcept {
    std::scoped_lock lk(_mutex);
    return AssetRefAny{this, LockLocked(handle)};
}

AssetRefAny AssetManager::GetAny(const AssetId& id) noexcept {
    std::scoped_lock lk(_mutex);
    return AssetRefAny{this, AcquireExistingLocked(id)};
}

AssetHandle AssetManager::LockLocked(AssetHandle handle) noexcept {
    unique_ptr<Slot>* slotPtr = _slots.TryGet(handle);
    if (slotPtr == nullptr) {
        return AssetHandle::Invalid();
    }
    Slot* slot = slotPtr->get();
    if (slot->State == AssetState::Free || slot->State == AssetState::Failed) {
        return AssetHandle::Invalid();
    }
    slot->StrongRefs.fetch_add(1, std::memory_order_relaxed);
    if (slot->State == AssetState::PendingRelease) {
        slot->State = AssetState::Loaded;
        auto pit = std::find(_pending.begin(), _pending.end(), handle);
        if (pit != _pending.end()) {
            _pending.erase(pit);
        }
    }
    return handle;
}

void AssetManager::CollectGarbage() {
    // 收集本轮要销毁的 slot,Asset 析构放到锁外执行(OnUnload 可能耗时)。
    vector<unique_ptr<Asset>> toDestroy;
    {
        std::scoped_lock lk(_mutex);
        for (AssetHandle handle : _pending) {
            unique_ptr<Slot>* slotPtr = _slots.TryGet(handle);
            if (slotPtr == nullptr) {
                continue;
            }
            Slot* slot = slotPtr->get();
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
            slot->State = AssetState::Free;
            slot->StrongRefs.store(0, std::memory_order_relaxed);
            _slots.Destroy(handle);
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
    return _slots.Count();
}

}  // namespace radray
