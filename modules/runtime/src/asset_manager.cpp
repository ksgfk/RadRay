#include <radray/runtime/asset_manager.h>

#include <algorithm>
#include <limits>

#include <radray/inline_vector.h>

namespace radray {
namespace detail {
struct AssetVersionNode {
    ErasedAsset Asset{nullptr, nullptr};
    InlineVector<UntypedAssetHandle, 4> Dependencies;
    Nullable<AssetVersionNode*> Next{nullptr};
};
}  // namespace detail

namespace {
template <typename Range>
bool ContainsHandle(const Range& range, UntypedAssetHandle handle) noexcept {
    return std::find(range.begin(), range.end(), handle) != range.end();
}
}  // namespace

class AssetManager::Storage {
public:
    struct Slot {
        AssetId Id{};
        std::type_index Type{typeid(void)};
        unique_ptr<detail::AssetVersionNode> Current;
        vector<UntypedAssetHandle> Dependents;
        uint32_t Generation{};
        uint32_t Revision{};
        uint32_t Refs{};
        uint32_t Next{UntypedAssetHandle::kInvalidIndex};
        uint32_t PreviousZero{UntypedAssetHandle::kInvalidIndex};
    };
    Nullable<const Slot*> Find(UntypedAssetHandle handle) const noexcept;
    vector<Slot> Slots;
    unordered_map<uint64_t, UntypedAssetHandle> Named;
    uint32_t FreeHead{UntypedAssetHandle::kInvalidIndex};
    uint32_t ZeroHead{UntypedAssetHandle::kInvalidIndex};
    size_t Retired{};
};

Nullable<const AssetManager::Storage::Slot*> AssetManager::Storage::Find(UntypedAssetHandle handle) const noexcept {
    if (handle.Index >= Slots.size()) return nullptr;
    const auto& slot = Slots[handle.Index];
    return slot.Revision && slot.Generation == handle.Generation ? &slot : nullptr;
}

AssetManager::AssetManager() : _storage(make_unique<Storage>()) {}
AssetManager::~AssetManager() noexcept {
    CollectGarbage();
    if (_storage->Retired) RADRAY_ABORT("AssetManager destroyed with retired versions");
    for (const auto& slot : _storage->Slots)
        if (slot.Current) RADRAY_ABORT("AssetManager destroyed with live asset id={} refs={}", slot.Id.Value, slot.Refs);
}
void AssetManager::CheckWritable() const noexcept {
    if (_inCommit) RADRAY_ABORT("AssetManager cannot be modified during commit preflight");
}
std::optional<UntypedAssetHandle> AssetManager::Find(AssetId id) const noexcept {
    const auto it = _storage->Named.find(id.Value);
    if (it == _storage->Named.end() || !_storage->Find(it->second)) return std::nullopt;
    return it->second;
}
bool AssetManager::Matches(UntypedAssetHandle target, std::type_index type) const noexcept {
    const auto slot = _storage->Find(target);
    return slot && slot->Type == type;
}
Nullable<const void*> AssetManager::Lookup(UntypedAssetHandle target, std::type_index type) const noexcept {
    const auto slot = _storage->Find(target);
    return slot && slot->Type == type ? slot->Current->Asset.get() : nullptr;
}
uint32_t AssetManager::GetRevision(UntypedAssetHandle target) const noexcept {
    const auto slot = _storage->Find(target);
    return slot ? slot->Revision : 0;
}
uint32_t AssetManager::GetRefCount(UntypedAssetHandle target) const noexcept {
    const auto slot = _storage->Find(target);
    return slot ? slot->Refs : 0;
}
std::span<const UntypedAssetHandle> AssetManager::GetDependents(UntypedAssetHandle target) const noexcept {
    const auto slot = _storage->Find(target);
    return slot ? std::span<const UntypedAssetHandle>{slot->Dependents} : std::span<const UntypedAssetHandle>{};
}
UntypedAssetHandle AssetManager::Reserve(AssetId id, std::type_index type, std::span<const UntypedAssetHandle> deps) {
    CheckWritable();
    if (id.IsValid() && _storage->Named.contains(id.Value)) return {};
    auto node = make_unique<detail::AssetVersionNode>();
    for (const auto dep : deps) {
        if (!_storage->Find(dep)) return {};
        if (!ContainsHandle(node->Dependencies, dep)) node->Dependencies.push_back(dep);
    }
    auto index = _storage->FreeHead;
    if (index == UntypedAssetHandle::kInvalidIndex) {
        if (_storage->Slots.size() >= index) RADRAY_ABORT("Asset slot indices exhausted");
        index = static_cast<uint32_t>(_storage->Slots.size());
        _storage->Slots.emplace_back();
    } else {
        _storage->FreeHead = std::exchange(_storage->Slots[index].Next, UntypedAssetHandle::kInvalidIndex);
    }
    auto& slot = _storage->Slots[index];
    const UntypedAssetHandle target{index, slot.Generation};
    slot.Type = type;
    slot.Id = id;
    slot.Refs = 1;
    slot.Current = std::move(node);
    for (const auto dep : slot.Current->Dependencies) Retain(dep);
    auto rollback = MakeScopeGuard([&]() noexcept { CancelReservation(target); });
    if (id.IsValid()) _storage->Named.emplace(id.Value, target);
    rollback.Dismiss();
    return target;
}
void AssetManager::Publish(UntypedAssetHandle target, detail::ErasedAsset value) {
    auto& slot = _storage->Slots[target.Index];
    for (const auto dep : slot.Current->Dependencies) {
        auto& parents = _storage->Slots[dep.Index].Dependents;
        parents.reserve(parents.size() + 1);
    }
    for (const auto dep : slot.Current->Dependencies) _storage->Slots[dep.Index].Dependents.push_back(target);
    slot.Current->Asset = std::move(value);
    slot.Revision = 1;
}
void AssetManager::FreeSlot(UntypedAssetHandle target) noexcept {
    auto& slot = _storage->Slots[target.Index];
    if (slot.Id.IsValid()) _storage->Named.erase(slot.Id.Value);
    if (slot.Generation == std::numeric_limits<uint32_t>::max()) RADRAY_ABORT("Asset slot generation exhausted");
    ++slot.Generation;
    slot.Revision = 0;
    slot.Refs = 0;
    slot.Next = std::exchange(_storage->FreeHead, target.Index);
}
void AssetManager::CancelReservation(UntypedAssetHandle target) noexcept {
    auto node = std::move(_storage->Slots[target.Index].Current);
    FreeSlot(target);
    for (const auto dep : node->Dependencies) Release(dep);
}
void AssetManager::Retain(UntypedAssetHandle target) noexcept {
    CheckWritable();
    auto& slot = _storage->Slots[target.Index];
    if (slot.Refs == std::numeric_limits<uint32_t>::max()) RADRAY_ABORT("Asset reference count exhausted");
    if (slot.Refs++ == 0) {
        if (slot.PreviousZero == UntypedAssetHandle::kInvalidIndex)
            _storage->ZeroHead = slot.Next;
        else
            _storage->Slots[slot.PreviousZero].Next = slot.Next;
        if (slot.Next != UntypedAssetHandle::kInvalidIndex) _storage->Slots[slot.Next].PreviousZero = slot.PreviousZero;
        slot.Next = slot.PreviousZero = UntypedAssetHandle::kInvalidIndex;
    }
}
void AssetManager::Release(UntypedAssetHandle target) noexcept {
    CheckWritable();
    auto& slot = _storage->Slots[target.Index];
    RADRAY_ASSERT(_storage->Find(target) && slot.Refs);
    if (--slot.Refs) return;
    if (HasPendingGarbage()) _storage->Slots[_storage->ZeroHead].PreviousZero = target.Index;
    slot.Next = std::exchange(_storage->ZeroHead, target.Index);
}
void AssetManager::DestroyNode(detail::AssetVersionNode& node) noexcept {
    ++_callbackDepth;
    node.Asset.reset();
    --_callbackDepth;
    for (const auto dep : node.Dependencies) Release(dep);
    --_storage->Retired;
}
void AssetManager::CheckBatch(const AssetGarbageBatch& retired) const noexcept {
    CheckWritable();
    if (_callbackDepth) RADRAY_ABORT("Reentrant asset collection or commit");
    if (!retired.Empty() && retired._manager.Get() != this) RADRAY_ABORT("Garbage batch belongs to another manager");
}
bool AssetManager::HasPendingGarbage() const noexcept { return _storage->ZeroHead != UntypedAssetHandle::kInvalidIndex; }
size_t AssetManager::CollectGarbage(AssetGarbageBatch& retired) noexcept {
    CheckBatch(retired);
    const auto before = retired.Size();
    while (HasPendingGarbage()) {
        const auto index = _storage->ZeroHead;
        auto& slot = _storage->Slots[index];
        _storage->ZeroHead = std::exchange(slot.Next, UntypedAssetHandle::kInvalidIndex);
        if (HasPendingGarbage()) _storage->Slots[_storage->ZeroHead].PreviousZero = UntypedAssetHandle::kInvalidIndex;
        RADRAY_ASSERT(slot.Refs == 0);
        const UntypedAssetHandle target{index, slot.Generation};
        for (const auto dep : slot.Current->Dependencies) std::erase(_storage->Slots[dep.Index].Dependents, target);
        retired.Append(*this, std::move(slot.Current));
        FreeSlot(target);
    }
    return retired.Size() - before;
}
size_t AssetManager::CollectGarbage() noexcept {
    AssetGarbageBatch batch;
    size_t count = 0;
    do {
        count += CollectGarbage(batch);
        batch.Clear();
    } while (HasPendingGarbage());
    return count;
}

AssetGarbageBatch::~AssetGarbageBatch() noexcept { Clear(); }
AssetGarbageBatch::AssetGarbageBatch(AssetGarbageBatch&& other) noexcept { *this = std::move(other); }
AssetGarbageBatch& AssetGarbageBatch::operator=(AssetGarbageBatch&& other) noexcept {
    if (this != &other) {
        Clear();
        _manager = other._manager.Release();
        _head = other._head.Release();
        _tail = other._tail.Release();
        _count = std::exchange(other._count, 0);
    }
    return *this;
}
void AssetGarbageBatch::Append(AssetManager& manager, unique_ptr<detail::AssetVersionNode> node) noexcept {
    _manager = &manager;
    auto* ptr = node.release();
    if (_tail)
        _tail->Next = ptr;
    else
        _head = ptr;
    _tail = ptr;
    ++_count;
    ++manager._storage->Retired;
}
void AssetGarbageBatch::Clear() noexcept {
    if (!_head) return;
    auto* manager = _manager.Get();
    manager->CheckWritable();
    // 分离整条链，析构回调不会再访问正在弹出的节点。
    auto next = _head;
    _head = _tail = nullptr;
    _manager = nullptr;
    _count = 0;
    while (next) {
        unique_ptr<detail::AssetVersionNode> node{next.Get()};
        next = node->Next;
        manager->DestroyNode(*node);
    }
}
bool AssetGarbageBatch::Empty() const noexcept { return _count == 0; }
size_t AssetGarbageBatch::Size() const noexcept { return _count; }

AssetTransaction::AssetTransaction(const AssetManager& manager) noexcept : _manager(&manager) {}
AssetTransaction::AssetTransaction(AssetTransaction&& other) noexcept { *this = std::move(other); }
AssetTransaction& AssetTransaction::operator=(AssetTransaction&& other) noexcept {
    CheckMutable();
    other.CheckMutable();
    if (this != &other) {
        _manager = other._manager.Release();
        _items = std::move(other._items);
        _diagnostic = std::move(other._diagnostic);
    }
    return *this;
}
void AssetTransaction::CheckMutable() const noexcept {
    if (_committing || (_manager && _manager->_inCommit)) RADRAY_ABORT("Cannot modify transaction during commit");
}
bool AssetTransaction::Fail(string diagnostic) {
    if (_diagnostic.empty()) _diagnostic = std::move(diagnostic);
    return false;
}
bool AssetTransaction::CheckOwner(Nullable<AssetManager*> owner) {
    CheckMutable();
    return (owner && _manager.Get() == owner.Get()) || Fail("asset reference belongs to another manager");
}
bool AssetTransaction::Validate(UntypedAssetHandle target, std::type_index type, std::span<const UntypedAssetHandle> deps) {
    CheckMutable();
    if (!_diagnostic.empty()) return false;
    if (!_manager || !_manager->Matches(target, type) || !_manager->GetRefCount(target)) return Fail("invalid replacement target");
    for (const auto& item : _items)
        if (item.Target == target) return Fail("duplicate replacement target");
    for (const auto dep : deps)
        if (dep == target || !_manager->GetRevision(dep)) return Fail("invalid replacement dependency");
    return true;
}
vector<UntypedAssetHandle> AssetTransaction::UniqueDependencies(std::span<const UntypedAssetHandle> deps) {
    vector<UntypedAssetHandle> result;
    for (const auto dep : deps)
        if (!ContainsHandle(result, dep)) result.push_back(dep);
    return result;
}
Nullable<const void*> AssetTransaction::Lookup(UntypedAssetHandle handle, std::type_index type) const noexcept {
    if (_manager && _manager->_building && !_evaluating) RADRAY_ABORT("Asset builder must use the active transaction");
    if (!_manager || (_evaluating && !ContainsHandle(_evaluating->Dependencies, handle))) return nullptr;
    for (const auto& item : _items)
        if (item.Target == handle) return item.Type == type ? item.Value.get() : nullptr;
    return _manager->Lookup(handle, type);
}

AssetCommitResult AssetManager::Commit(AssetTransaction&& tx, AssetGarbageBatch& retired) {
    CheckBatch(retired);
    if (tx._manager.Get() != this) return {false, {}, "transaction belongs to another manager or was consumed"};
    tx.CheckMutable();
    tx._committing = true;
    vector<size_t> publishOrder;
    auto consume = MakeScopeGuard([&]() noexcept {
        ++_callbackDepth;
        // 父候选可借用已构建的子候选，失败时仍按父先子后释放。
        for (auto it = publishOrder.rbegin(); it != publishOrder.rend(); ++it) tx._items[*it].Value.reset();
        tx._items.clear();
        --_callbackDepth;
        tx._manager = nullptr;
        tx._committing = false;
    });
    if (!tx._diagnostic.empty()) return {false, {}, std::move(tx._diagnostic)};
    if (tx._items.empty()) return {true, {}, {}};

    const auto absent = tx._items.size();
    vector<size_t> staged(_storage->Slots.size(), absent);
    for (size_t i = 0; i < tx._items.size(); ++i) {
        auto& item = tx._items[i];
        if (!Matches(item.Target, item.Type) || !GetRefCount(item.Target)) return {false, {}, "replacement target expired"};
        for (const auto dep : item.Dependencies)
            if (!GetRevision(dep) || dep == item.Target) return {false, {}, "replacement dependency expired"};
        staged[item.Target.Index] = i;
    }

    // 完整图的后序同时保留经未暂存槽位连接的传递顺序。显式栈允许任意深度。
    const auto sort = [&](bool proposed, vector<size_t>& order) {
        struct Visit {
            uint32_t Index;
            size_t Next{};
        };
        vector<Visit> stack;
        vector<uint8_t> color(staged.size(), 0);
        order.reserve(tx._items.size());
        for (const auto& item : tx._items) {
            if (color[item.Target.Index]) continue;
            stack.push_back({item.Target.Index});
            color[item.Target.Index] = 1;
            while (!stack.empty()) {
                auto& visit = stack.back();
                const auto index = staged[visit.Index];
                const std::span<const UntypedAssetHandle> deps = proposed && index != absent
                                                                     ? std::span<const UntypedAssetHandle>{tx._items[index].Dependencies}
                                                                     : std::span<const UntypedAssetHandle>{_storage->Slots[visit.Index].Current->Dependencies};
                if (visit.Next == deps.size()) {
                    color[visit.Index] = 2;
                    if (index != absent) order.push_back(index);
                    stack.pop_back();
                    continue;
                }
                const auto child = deps[visit.Next++].Index;
                if (color[child] == 1) return false;
                if (!color[child]) {
                    color[child] = 1;
                    stack.push_back({child});
                }
            }
        }
        return true;
    };
    if (!sort(true, publishOrder)) return {false, {}, "cyclic asset dependencies"};
    vector<size_t> retireOrder;
    if (!sort(false, retireOrder)) RADRAY_ABORT("Live asset dependency cycle");

    {
        _inCommit = true;
        auto unlock = MakeScopeGuard([&]() noexcept { _inCommit = false; });
        for (const auto index : publishOrder) {
            auto& item = tx._items[index];
            if (!item.Build) continue;
            _building = true;
            tx._evaluating = &item;
            auto endBuild = MakeScopeGuard([&]() noexcept {
                tx._evaluating = nullptr;
                _building = false;
            });
            item.Value = item.Build(item.Builder.get(), tx);
            if (!item.Value) return {false, {}, "asset builder rejected candidate"};
        }
        for (auto& item : tx._items) {
            const auto& slot = _storage->Slots[item.Target.Index];
            auto decision = item.Classify(slot.Current->Asset.get(), item.Value.get());
            item.Kind = decision.Kind;
            if (item.Kind == AssetReloadKind::Rejected) return {false, {}, std::move(decision.Diagnostic)};
            if (item.Kind == AssetReloadKind::Breaking) {
                for (const auto parent : slot.Dependents)
                    if (staged[parent.Index] == absent) return {false, {}, "breaking replacement omits a live dependent: " + decision.Diagnostic};
            }
        }
    }

    // 回调可以通过外部引用移动批次，切换前再次确认退役链的归属。
    CheckBatch(retired);
    vector<unique_ptr<detail::AssetVersionNode>> next;
    next.reserve(tx._items.size());
    vector<size_t> additions(staged.size(), 0);
    for (const auto& item : tx._items) {
        const auto& slot = _storage->Slots[item.Target.Index];
        if (slot.Revision == std::numeric_limits<uint32_t>::max()) RADRAY_ABORT("Asset revision exhausted");
        auto node = make_unique<detail::AssetVersionNode>();
        node->Dependencies.assign(item.Dependencies.begin(), item.Dependencies.end());
        for (const auto dep : item.Dependencies)
            if (!ContainsHandle(slot.Current->Dependencies, dep)) ++additions[dep.Index];
        next.push_back(std::move(node));
    }
    AssetCommitResult result{true, {}, {}};
    result.Events.reserve(next.size());
    for (size_t i = 0; i < additions.size(); ++i) {
        if (!additions[i]) continue;
        auto& parents = _storage->Slots[i].Dependents;
        parents.reserve(parents.size() + additions[i]);
    }

    for (auto it = retireOrder.rbegin(); it != retireOrder.rend(); ++it) {
        const auto i = *it;
        auto& item = tx._items[i];
        auto& slot = _storage->Slots[item.Target.Index];
        const auto& old = slot.Current->Dependencies;
        for (const auto dep : old)
            if (!ContainsHandle(item.Dependencies, dep)) std::erase(_storage->Slots[dep.Index].Dependents, item.Target);
        for (const auto dep : item.Dependencies) {
            Retain(dep);
            if (!ContainsHandle(old, dep)) _storage->Slots[dep.Index].Dependents.push_back(item.Target);
        }
        next[i]->Asset = std::move(item.Value);
        retired.Append(*this, std::exchange(slot.Current, std::move(next[i])));
        ++slot.Revision;
    }
    for (const auto i : publishOrder) {
        const auto& item = tx._items[i];
        const auto& slot = _storage->Slots[item.Target.Index];
        result.Events.push_back({item.Target, slot.Id, slot.Type, slot.Revision, item.Kind});
    }
    return result;
}
}  // namespace radray
