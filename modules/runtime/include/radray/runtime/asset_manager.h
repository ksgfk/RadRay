#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <typeindex>
#include <type_traits>
#include <utility>

#include <radray/logger.h>
#include <radray/nullable.h>
#include <radray/runtime/asset_handle.h>
#include <radray/scope_guard.h>
#include <radray/types.h>

namespace radray {

enum class AssetReloadKind : uint8_t { Compatible,
                                       Breaking,
                                       Rejected };

struct AssetReloadDecision {
    AssetReloadKind Kind{AssetReloadKind::Compatible};
    string Diagnostic{};
};
struct AssetReloadEvent {
    UntypedAssetHandle Handle{};
    AssetId Id{};
    std::type_index Type{typeid(void)};
    uint32_t Revision{};
    AssetReloadKind Kind{AssetReloadKind::Compatible};
};
struct AssetCommitResult {
    bool Succeeded{};
    vector<AssetReloadEvent> Events;
    string Diagnostic{};
};

namespace detail {
using ErasedAsset = unique_ptr<void, void (*)(void*) noexcept>;
template <typename T>
ErasedAsset OwnAsset(T&& value) {
    using Value = std::decay_t<T>;
    return {new Value(std::forward<T>(value)), [](void* ptr) noexcept { delete static_cast<Value*>(ptr); }};
}
struct AssetVersionNode;
}  // namespace detail

class AssetTransaction;

// 移动独占所有权。非空批次必须在其 manager 之前清理或销毁。
class AssetGarbageBatch final {
public:
    AssetGarbageBatch() noexcept = default;
    ~AssetGarbageBatch() noexcept;
    AssetGarbageBatch(AssetGarbageBatch&& other) noexcept;
    AssetGarbageBatch& operator=(AssetGarbageBatch&& other) noexcept;
    AssetGarbageBatch(const AssetGarbageBatch&) = delete;
    AssetGarbageBatch& operator=(const AssetGarbageBatch&) = delete;
    void Clear() noexcept;
    bool Empty() const noexcept;
    size_t Size() const noexcept;

private:
    friend class AssetManager;
    void Append(AssetManager& manager, unique_ptr<detail::AssetVersionNode> node) noexcept;
    Nullable<AssetManager*> _manager{nullptr};
    Nullable<detail::AssetVersionNode*> _head{nullptr};
    Nullable<detail::AssetVersionNode*> _tail{nullptr};
    size_t _count{};
};

// 所有操作限 owner 线程。Get 借用当前版本，跨提交使用旧对象须单独保活。
class AssetManager final {
public:
    AssetManager();
    ~AssetManager() noexcept;
    AssetManager(const AssetManager&) = delete;
    AssetManager& operator=(const AssetManager&) = delete;

    std::optional<UntypedAssetHandle> Find(AssetId id) const noexcept;
    template <typename T>
    AssetHandle<T> AsHandle(UntypedAssetHandle handle) const noexcept {
        return Matches(handle, typeid(T)) ? AssetHandle<T>{handle} : AssetHandle<T>{};
    }
    template <typename T>
    AssetHandle<T> Find(AssetId id) const noexcept {
        const auto found = Find(id);
        return found ? AsHandle<T>(*found) : AssetHandle<T>{};
    }
    template <typename T, typename Factory>
    AssetRef<T> EmplaceWithHandle(std::span<const UntypedAssetHandle> deps, AssetId id, Factory&& factory) {
        const auto target = Reserve(id, typeid(T), deps);
        if (!target.IsValid()) return {};
        auto rollback = MakeScopeGuard([&]() noexcept { CancelReservation(target); });
        const AssetHandle<T> handle{target};
        ++_callbackDepth;
        auto callback = MakeScopeGuard([this]() noexcept { --_callbackDepth; });
        std::optional<T> value = std::forward<Factory>(factory)(handle);
        if (!value) return {};
        Publish(target, detail::OwnAsset(std::move(*value)));
        rollback.Dismiss();
        return AssetRef<T>{*this, handle};
    }
    template <typename T>
    AssetRef<std::remove_cvref_t<T>> Emplace(T&& value, std::span<const UntypedAssetHandle> deps = {}, AssetId id = {}) {
        using Value = std::remove_cvref_t<T>;
        return EmplaceWithHandle<Value>(deps, id, [&](AssetHandle<Value>) {
            return std::optional<Value>{std::in_place, std::forward<T>(value)};
        });
    }
    template <typename T>
    AssetRef<T> Acquire(AssetHandle<T> handle) noexcept {
        CheckWritable();
        if (!Matches(handle.Untyped(), typeid(T))) return {};
        Retain(handle.Untyped());
        return AssetRef<T>{*this, handle};
    }
    template <typename T>
    Nullable<const T*> Get(AssetHandle<T> handle) const noexcept {
        if (_building) RADRAY_ABORT("Asset builder must resolve dependencies through its transaction");
        return static_cast<const T*>(Lookup(handle.Untyped(), typeid(T)).Get());
    }
    uint32_t GetRefCount(UntypedAssetHandle handle) const noexcept;
    uint32_t GetRevision(UntypedAssetHandle handle) const noexcept;
    // 修改仓库后须重新获取该视图。
    std::span<const UntypedAssetHandle> GetDependents(UntypedAssetHandle handle) const noexcept;
    // 成功时返回新图子先父后的事件，并按旧图父先子后追加退役版本。
    AssetCommitResult Commit(AssetTransaction&& tx, AssetGarbageBatch& retired);
    bool HasPendingGarbage() const noexcept;
    size_t CollectGarbage(AssetGarbageBatch& retired) noexcept;
    size_t CollectGarbage() noexcept;

private:
    template <typename T>
    friend class AssetRef;
    friend class AssetTransaction;
    friend class AssetGarbageBatch;
    class Storage;
    void CheckWritable() const noexcept;
    bool Matches(UntypedAssetHandle target, std::type_index type) const noexcept;
    Nullable<const void*> Lookup(UntypedAssetHandle target, std::type_index type) const noexcept;
    UntypedAssetHandle Reserve(AssetId id, std::type_index type, std::span<const UntypedAssetHandle> deps);
    void Publish(UntypedAssetHandle target, detail::ErasedAsset value);
    void CancelReservation(UntypedAssetHandle target) noexcept;
    void Retain(UntypedAssetHandle target) noexcept;
    void Release(UntypedAssetHandle target) noexcept;
    void FreeSlot(UntypedAssetHandle target) noexcept;
    void DestroyNode(detail::AssetVersionNode& node) noexcept;
    void CheckBatch(const AssetGarbageBatch& retired) const noexcept;
    unique_ptr<Storage> _storage;
    size_t _callbackDepth{};
    bool _inCommit{};
    bool _building{};
};

template <typename T>
class AssetRef {
public:
    AssetRef() noexcept = default;
    ~AssetRef() noexcept { Reset(); }
    AssetRef(const AssetRef& other) noexcept : _manager(other._manager), _handle(other._handle) {
        if (_manager) _manager->Retain(Untyped());
    }
    AssetRef(AssetRef&& other) noexcept : _manager(other._manager.Release()), _handle(std::exchange(other._handle, {})) {}
    AssetRef& operator=(const AssetRef& other) noexcept {
        if (this != &other) *this = AssetRef(other);
        return *this;
    }
    AssetRef& operator=(AssetRef&& other) noexcept {
        if (this != &other) {
            Reset();
            _manager = other._manager.Release();
            _handle = std::exchange(other._handle, {});
        }
        return *this;
    }
    void Reset() noexcept {
        if (_manager) _manager.Release()->Release(std::exchange(_handle, {}).Untyped());
    }
    bool IsValid() const noexcept { return _manager.HasValue(); }
    explicit operator bool() const noexcept { return IsValid(); }
    AssetHandle<T> GetHandle() const noexcept { return _handle; }
    operator AssetHandle<T>() const noexcept { return _handle; }
    UntypedAssetHandle Untyped() const noexcept { return _handle.Untyped(); }
    Nullable<const T*> Get() const noexcept { return _manager ? _manager->Get(_handle) : nullptr; }

private:
    friend class AssetManager;
    friend class AssetTransaction;
    AssetRef(AssetManager& manager, AssetHandle<T> handle) noexcept : _manager(&manager), _handle(handle) {}
    Nullable<AssetManager*> _manager{nullptr};
    AssetHandle<T> _handle{};
};

class AssetTransaction final {
public:
    explicit AssetTransaction(const AssetManager& manager) noexcept;
    AssetTransaction(AssetTransaction&& other) noexcept;
    AssetTransaction& operator=(AssetTransaction&& other) noexcept;
    AssetTransaction(const AssetTransaction&) = delete;
    AssetTransaction& operator=(const AssetTransaction&) = delete;

    template <typename T>
    bool StageReplace(AssetHandle<T> target, T&& candidate, std::span<const UntypedAssetHandle> deps = {}) {
        if (!Validate(target.Untyped(), typeid(T), deps)) return false;
        Item item{target.Untyped(), typeid(T), UniqueDependencies(deps), &Classify<T>};
        item.Value = detail::OwnAsset(std::move(candidate));
        _items.push_back(std::move(item));
        return true;
    }
    template <typename T>
    bool StageReplace(const AssetRef<T>& target, T&& candidate, std::span<const UntypedAssetHandle> deps = {}) {
        return CheckOwner(target._manager) && StageReplace(target.GetHandle(), std::move(candidate), deps);
    }
    // builder 在提交预检时执行，只能通过传入事务读取 deps 中的最终版本。
    template <typename T, typename Builder>
    bool StageBuild(AssetHandle<T> target, std::span<const UntypedAssetHandle> deps, Builder&& builder) {
        if (!Validate(target.Untyped(), typeid(T), deps)) return false;
        Item item{target.Untyped(), typeid(T), UniqueDependencies(deps), &Classify<T>};
        item.Builder = detail::OwnAsset(std::forward<Builder>(builder));
        item.Build = [](void* erased, const AssetTransaction& tx) -> detail::ErasedAsset {
            std::optional<T> value = (*static_cast<std::decay_t<Builder>*>(erased))(tx);
            return value ? detail::OwnAsset(std::move(*value)) : detail::ErasedAsset{nullptr, nullptr};
        };
        _items.push_back(std::move(item));
        return true;
    }
    template <typename T, typename Builder>
    bool StageBuild(const AssetRef<T>& target, std::span<const UntypedAssetHandle> deps, Builder&& builder) {
        return CheckOwner(target._manager) && StageBuild(target.GetHandle(), deps, std::forward<Builder>(builder));
    }
    template <typename T>
    Nullable<const T*> Get(AssetHandle<T> handle) const noexcept {
        return static_cast<const T*>(Lookup(handle.Untyped(), typeid(T)).Get());
    }

private:
    friend class AssetManager;
    struct Item {
        UntypedAssetHandle Target;
        std::type_index Type;
        vector<UntypedAssetHandle> Dependencies;
        AssetReloadDecision (*Classify)(const void*, const void*);
        detail::ErasedAsset Value{nullptr, nullptr};
        detail::ErasedAsset Builder{nullptr, nullptr};
        detail::ErasedAsset (*Build)(void*, const AssetTransaction&){};
        AssetReloadKind Kind{};
    };
    template <typename T>
    static AssetReloadDecision Classify(const void* before, const void* after) {
        if constexpr (requires(const T& a, const T& b) { { a.ClassifyReload(b) } -> std::same_as<AssetReloadDecision>; }) {
            return static_cast<const T*>(before)->ClassifyReload(*static_cast<const T*>(after));
        } else {
            return {};
        }
    }
    bool CheckOwner(Nullable<AssetManager*> owner);
    void CheckMutable() const noexcept;
    bool Fail(string diagnostic);
    bool Validate(UntypedAssetHandle target, std::type_index type, std::span<const UntypedAssetHandle> deps);
    static vector<UntypedAssetHandle> UniqueDependencies(std::span<const UntypedAssetHandle> deps);
    Nullable<const void*> Lookup(UntypedAssetHandle handle, std::type_index type) const noexcept;
    Nullable<const AssetManager*> _manager{nullptr};
    vector<Item> _items;
    string _diagnostic;
    Nullable<const Item*> _evaluating{nullptr};
    bool _committing{};
};

}  // namespace radray
