#include <radray/runtime/asset_manager.h>

#include <algorithm>
#include <limits>

#include <radray/inline_vector.h>

namespace radray {

class AssetManager::Storage {
public:
    struct Slot {
        AssetId Id{};
        std::type_index Type{typeid(void)};
        detail::ErasedAsset Asset{nullptr, nullptr};
        InlineVector<UntypedAssetHandle, 4> Dependencies;
        AssetState State{AssetState::Unloaded};
        uint32_t Generation{};
        uint32_t Refs{};
        uint32_t Next{UntypedAssetHandle::kInvalidIndex};
        bool Alive{};
        bool Queued{};
    };
    struct TypedMeta {
        std::type_index Type;
        AssetMeta Meta;
    };
    struct WaitRecord : ManualCoroutineRecord {
        WaitRecord(AssetManager& owner, UntypedAssetHandle target, bool dependencies) noexcept
            : Owner(owner), Target(target), Dependencies(dependencies) {}
        void ResumeContinuation(std::coroutine_handle<> continuation) noexcept {
            Owner.DispatchExternal([continuation]() noexcept { continuation.resume(); });
        }
        AssetManager& Owner;
        UntypedAssetHandle Target;
        bool Dependencies;
    };

    Nullable<const Slot*> Find(UntypedAssetHandle target) const noexcept {
        if (target.GetIndex() >= Slots.size()) return nullptr;
        const auto& slot = Slots[target.GetIndex()];
        return slot.Alive && slot.Generation == target.GetGeneration() ? &slot : nullptr;
    }
    bool HasPath(UntypedAssetHandle start, UntypedAssetHandle target) const {
        vector<UntypedAssetHandle> work{start};
        unordered_set<uint32_t> visited;
        while (!work.empty()) {
            const auto handle = work.back();
            work.pop_back();
            if (handle == target) return true;
            if (!visited.insert(handle.GetIndex()).second) continue;
            const auto& deps = Slots[handle.GetIndex()].Dependencies;
            work.insert(work.end(), deps.begin(), deps.end());
        }
        return false;
    }

    unordered_map<std::type_index, unique_ptr<detail::ErasedAssetLoader>> Loaders;
    unordered_map<uint64_t, TypedMeta> Metas;
    unordered_map<uint64_t, UntypedAssetHandle> Named;
    vector<Slot> Slots;
    deque<UntypedAssetHandle> Pending;
    ManualCoroutineScheduler<WaitRecord> Waiters;
    uint32_t FreeHead{UntypedAssetHandle::kInvalidIndex};
    uint32_t ZeroHead{UntypedAssetHandle::kInvalidIndex};
};

AssetManager::AssetManager() : _storage(make_unique<Storage>()) {}

AssetManager::~AssetManager() noexcept {
    Shutdown();
    {
        auto callback = EnterCallback();
        while (!_storage->Loaders.empty()) _storage->Loaders.extract(_storage->Loaders.begin());
    }
    CollectGarbage();
    for (const auto& slot : _storage->Slots) {
        if (slot.Alive) RADRAY_ABORT("AssetManager destroyed with live asset id={} refs={}", slot.Id.Value, slot.Refs);
    }
}

void AssetManager::AddLoader(std::type_index type, unique_ptr<detail::ErasedAssetLoader> loader) {
    if (!_storage->Loaders.try_emplace(type, std::move(loader)).second)
        RADRAY_ABORT("Cannot register asset loader for {}", type.name());
}

bool AssetManager::AddMeta(std::type_index type, AssetMeta meta) {
    if (_shuttingDown || !meta.Id.IsValid()) return false;
    const auto id = meta.Id.Value;
    const auto named = _storage->Named.find(id);
    if (named != _storage->Named.end() && _storage->Slots[named->second.GetIndex()].Type != type) return false;
    return _storage->Metas.try_emplace(id, Storage::TypedMeta{type, std::move(meta)}).second;
}

Nullable<const AssetMeta*> AssetManager::FindMeta(AssetId id) const noexcept {
    const auto it = _storage->Metas.find(id.Value);
    return it == _storage->Metas.end() ? nullptr : &it->second.Meta;
}

std::optional<UntypedAssetHandle> AssetManager::FindLoaded(AssetId id) const noexcept {
    const auto it = _storage->Named.find(id.Value);
    if (it == _storage->Named.end() || !_storage->Find(it->second)) return std::nullopt;
    return it->second;
}

bool AssetManager::Matches(UntypedAssetHandle target, std::type_index type) const noexcept {
    const auto slot = _storage->Find(target);
    return slot && slot->Type == type;
}

AssetState AssetManager::GetState(UntypedAssetHandle target) const noexcept {
    const auto slot = _storage->Find(target);
    return slot ? slot->State : AssetState::Unloaded;
}

uint32_t AssetManager::GetRefCount(UntypedAssetHandle target) const noexcept {
    const auto slot = _storage->Find(target);
    return slot ? slot->Refs : 0;
}

Nullable<const void*> AssetManager::Lookup(UntypedAssetHandle target, std::type_index type, bool fallback) const noexcept {
    const auto slot = _storage->Find(target);
    if (slot && slot->Type == type && slot->State == AssetState::Ready) return slot->Asset.get();
    if (!fallback) return nullptr;
    const auto it = _storage->Loaders.find(type);
    if (it == _storage->Loaders.end()) return nullptr;
    auto callback = EnterCallback();
    return it->second->Fallback();
}

UntypedAssetHandle AssetManager::Reserve(AssetId id, std::type_index type) {
    if (_shuttingDown) return {};
    if (id.IsValid()) {
        if (_storage->Named.contains(id.Value)) return {};
        const auto meta = _storage->Metas.find(id.Value);
        if (meta != _storage->Metas.end() && meta->second.Type != type) return {};
    }

    auto index = _storage->FreeHead;
    if (index == UntypedAssetHandle::kInvalidIndex) {
        if (_storage->Slots.size() >= index) RADRAY_ABORT("Asset slot indices exhausted");
        index = static_cast<uint32_t>(_storage->Slots.size());
        _storage->Slots.emplace_back();
    } else {
        _storage->FreeHead = _storage->Slots[index].Next;
    }
    auto& slot = _storage->Slots[index];
    slot.Next = UntypedAssetHandle::kInvalidIndex;
    slot.Id = id;
    slot.Type = type;
    slot.Refs = 1;
    const UntypedAssetHandle target{index, slot.Generation};
    auto rollback = MakeScopeGuard([&]() noexcept { Recycle(target); });
    if (id.IsValid()) _storage->Named.emplace(id.Value, target);
    rollback.Dismiss();
    return target;
}

UntypedAssetHandle AssetManager::RequestSlot(AssetId id, std::type_index type) {
    if (_shuttingDown || !id.IsValid()) return {};
    const auto named = _storage->Named.find(id.Value);
    if (named != _storage->Named.end()) {
        const auto target = named->second;
        if (!Matches(target, type)) return {};
        Retain(target);
        return target;
    }
    const auto meta = _storage->Metas.find(id.Value);
    if (meta == _storage->Metas.end() || meta->second.Type != type || !_storage->Loaders.contains(type)) return {};
    const auto target = Reserve(id, type);
    auto rollback = MakeScopeGuard([&]() noexcept { Recycle(target); });
    _storage->Pending.push_back(target);
    auto& slot = _storage->Slots[target.GetIndex()];
    slot.State = AssetState::Loading;
    slot.Alive = true;
    rollback.Dismiss();
    return target;
}

void AssetManager::Retain(UntypedAssetHandle target) noexcept {
    auto& refs = _storage->Slots[target.GetIndex()].Refs;
    if (refs == std::numeric_limits<uint32_t>::max()) RADRAY_ABORT("Asset reference count exhausted");
    ++refs;
}

void AssetManager::Release(UntypedAssetHandle target) noexcept {
    RADRAY_ASSERT(_storage->Find(target) && GetRefCount(target) != 0);
    --_storage->Slots[target.GetIndex()].Refs;
    QueueIfUnused(target);
}

void AssetManager::QueueIfUnused(UntypedAssetHandle target) noexcept {
    auto& slot = _storage->Slots[target.GetIndex()];
    if (slot.Refs || slot.State == AssetState::Loading || slot.Queued) return;
    slot.Next = _storage->ZeroHead;
    slot.Queued = true;
    _storage->ZeroHead = target.GetIndex();
}

void AssetManager::AddDependency(UntypedAssetHandle parent, UntypedAssetHandle child) {
    auto& deps = _storage->Slots[parent.GetIndex()].Dependencies;
    if (std::find(deps.begin(), deps.end(), child) != deps.end()) return;
    deps.push_back(child);
    Retain(child);
}

UntypedAssetHandle AssetManager::RequestDependency(UntypedAssetHandle parent, AssetId id, std::optional<std::type_index> type) {
    if (!type) {
        if (const auto published = FindLoaded(id)) {
            type = _storage->Slots[published->GetIndex()].Type;
        } else {
            const auto meta = _storage->Metas.find(id.Value);
            if (meta == _storage->Metas.end()) return {};
            type = meta->second.Type;
        }
    }
    const auto child = RequestSlot(id, *type);
    if (!child.IsValid()) return {};
    auto release = MakeScopeGuard([&]() noexcept { Release(child); });
    if (_storage->HasPath(child, parent)) return {};
    AddDependency(parent, child);
    return child;
}

UntypedAssetHandle AssetManager::FindDependency(UntypedAssetHandle parent, AssetId id, std::type_index type) const noexcept {
    const auto child = FindLoaded(id);
    if (!child || !Matches(*child, type)) return {};
    const auto& deps = _storage->Slots[parent.GetIndex()].Dependencies;
    return std::find(deps.begin(), deps.end(), *child) != deps.end() ? *child : UntypedAssetHandle{};
}

AssetState AssetManager::DependencyState(UntypedAssetHandle parent) const noexcept {
    auto result = AssetState::Ready;
    for (const auto child : _storage->Slots[parent.GetIndex()].Dependencies) {
        const auto state = GetState(child);
        if (state == AssetState::Failed) return state;
        if (state == AssetState::Loading) result = state;
    }
    return result;
}

void AssetManager::ReleaseDependencies(UntypedAssetHandle target) noexcept {
    auto deps = std::move(_storage->Slots[target.GetIndex()].Dependencies);
    for (const auto child : deps) Release(child);
}

void AssetManager::Recycle(UntypedAssetHandle target) noexcept {
    const auto index = target.GetIndex();
    auto& slot = _storage->Slots[index];
    if (slot.Id.IsValid()) _storage->Named.erase(slot.Id.Value);
    slot.Alive = false;
    slot.State = AssetState::Unloaded;
    slot.Refs = 0;
    if (slot.Generation == std::numeric_limits<uint32_t>::max()) RADRAY_ABORT("Asset slot generation exhausted");
    ++slot.Generation;
    auto deps = std::move(slot.Dependencies);
    auto value = std::move(slot.Asset);
    {
        auto callback = EnterCallback();
        value.reset();
    }
    for (const auto child : deps) Release(child);
    _storage->Slots[index].Next = _storage->FreeHead;
    _storage->FreeHead = index;
}

size_t AssetManager::CollectGarbage() noexcept {
    size_t collected = 0;
    while (_storage->ZeroHead != UntypedAssetHandle::kInvalidIndex) {
        const auto index = _storage->ZeroHead;
        auto& slot = _storage->Slots[index];
        _storage->ZeroHead = std::exchange(slot.Next, UntypedAssetHandle::kInvalidIndex);
        slot.Queued = false;
        if (slot.Refs != 0) continue;
        Recycle({index, slot.Generation});
        ++collected;
    }
    return collected;
}

void AssetManager::Publish(UntypedAssetHandle target, detail::ErasedAsset value) noexcept {
    auto& slot = _storage->Slots[target.GetIndex()];
    slot.Asset = std::move(value);
    slot.Alive = true;
    slot.State = AssetState::Ready;
    QueueIfUnused(target);
}

void AssetManager::FailSlot(UntypedAssetHandle target) noexcept {
    if (GetState(target) != AssetState::Loading) return;
    _storage->Slots[target.GetIndex()].State = AssetState::Failed;
    ReleaseDependencies(target);
    QueueIfUnused(target);
}

class AssetManager::SlotAwaiter {
public:
    AssetManager& Manager;
    UntypedAssetHandle Target;
    stop_token Stop;
    bool Dependencies;
    Nullable<Storage::WaitRecord*> Record{nullptr};

    bool await_ready() const noexcept {
        return Stop.stop_requested() || (Dependencies ? Manager.DependencyState(Target) : Manager.GetState(Target)) != AssetState::Loading;
    }
    void await_suspend(std::coroutine_handle<> continuation) {
        Record = Manager._storage->Waiters.Enqueue(Stop, continuation, Manager, Target, Dependencies);
    }
    bool await_resume() noexcept {
        const bool canceled = Stop.stop_requested() || (Record && Record->Canceled);
        if (Record) Manager._storage->Waiters.Erase(Record.Get());
        return !canceled;
    }
};

task<void> AssetManager::WaitFor(UntypedAssetHandle target, stop_token stop, bool dependencies) {
    if (!co_await SlotAwaiter{*this, target, stop, dependencies}) co_await StopCurrentTask();
}

task<void> AssetManager::RunSlotLoad(UntypedAssetHandle target) {
    auto fail = MakeScopeGuard([&]() noexcept { FailSlot(target); });
    const auto& meta = _storage->Metas.at(_storage->Slots[target.GetIndex()].Id.Value);
    for (const auto id : meta.Meta.Dependencies) {
        if (!RequestDependency(target, id, std::nullopt).IsValid()) co_return;
    }
    co_await WaitFor(target, _loadScope.GetStopToken(), true);
    if (DependencyState(target) != AssetState::Ready) co_return;
    AssetLoadContext context{*this, target, meta.Meta};
    auto asset = co_await _storage->Loaders.at(meta.Type)->Read(context);
    if (!asset || context._dependencyFailed || _shuttingDown) co_return;
    Publish(target, std::move(asset));
    fail.Dismiss();
}

bool AssetManager::HasPendingWork() const noexcept {
    return !_storage->Waiters.Empty() || std::any_of(_storage->Slots.begin(), _storage->Slots.end(),
                                                     [](const auto& slot) { return slot.State == AssetState::Loading; });
}

void AssetManager::Pump() {
    if (_callbackDepth) RADRAY_ABORT("Reentrant AssetManager::Pump");
    if (_shuttingDown) return;
    auto callback = EnterCallback();
    for (;;) {
        const auto count = _storage->Pending.size();
        for (size_t i = 0; i < count; ++i) {
            const auto target = _storage->Pending.front();
            _storage->Pending.pop_front();
            auto rollback = MakeScopeGuard([&]() noexcept { FailSlot(target); });
            _loadScope.Spawn(RunSlotLoad(target));
            rollback.Dismiss();
        }
        auto ready = [this](const Storage::WaitRecord& record) noexcept {
            return (record.Dependencies ? DependencyState(record.Target) : GetState(record.Target)) != AssetState::Loading;
        };
        const auto dispatched = _storage->Waiters.DispatchReady(ready, _storage->Waiters.GetSequenceBoundary());
        if (!count && !dispatched) return;
    }
}

void AssetManager::Shutdown() noexcept {
    if (_callbackDepth) RADRAY_ABORT("AssetManager::Shutdown cannot be called reentrantly");
    if (_shuttingDown) return;
    _shuttingDown = true;
    auto callback = EnterCallback();
    for (const auto target : _storage->Pending) FailSlot(target);
    _storage->Pending.clear();
    _loadScope.RequestStop();
    _storage->Waiters.CancelAll();
    _loadScope.WaitUntilEmpty();
}

}  // namespace radray
