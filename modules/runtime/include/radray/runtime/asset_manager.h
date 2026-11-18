#pragma once

#include <cstddef>
#include <concepts>
#include <filesystem>
#include <optional>
#include <span>
#include <typeindex>
#include <type_traits>
#include <utility>

#include <radray/logger.h>
#include <radray/types.h>
#include <radray/coroutine.h>
#include <radray/nullable.h>
#include <radray/scope_guard.h>
#include <radray/runtime/asset_handle.h>

namespace radray {

struct AssetMeta {
    AssetId Id{};
    std::filesystem::path Path;
    vector<AssetId> Dependencies;
};

namespace detail {
using ErasedAsset = unique_ptr<void, void (*)(void*) noexcept>;

template <typename T>
ErasedAsset OwnAsset(T&& value) {
    using Value = std::remove_cvref_t<T>;
    return {new Value(std::forward<T>(value)), [](void* ptr) noexcept { delete static_cast<Value*>(ptr); }};
}

class ErasedAssetLoader {
public:
    virtual ~ErasedAssetLoader() noexcept = default;
    virtual task<ErasedAsset> Read(AssetLoadContext& context) = 0;
    virtual Nullable<const void*> Fallback() const noexcept = 0;
};
}  // namespace detail

template <typename T>
class AssetLoader : public detail::ErasedAssetLoader {
public:
    // 外部挂起点须在 owner 线程响应 stop，关闭期间无需再次 Pump。
    virtual task<std::optional<T>> LoadAsync(AssetLoadContext& context) = 0;
    virtual Nullable<const T*> GetFallback() const noexcept { return nullptr; }

private:
    task<detail::ErasedAsset> Read(AssetLoadContext& context) final;
    Nullable<const void*> Fallback() const noexcept final { return GetFallback().Get(); }
};

// 创建、等待、复制/释放强引用和请求取消均由 owner 线程串行执行。
class AssetManager final {
    auto EnterCallback() const noexcept {
        ++_callbackDepth;
        return MakeScopeGuard([this]() noexcept { --_callbackDepth; });
    }

public:
    AssetManager();
    ~AssetManager() noexcept;
    AssetManager(const AssetManager&) = delete;
    AssetManager& operator=(const AssetManager&) = delete;

    template <typename T, typename LoaderT, typename... Args>
    LoaderT& RegisterLoader(Args&&... args) {
        static_assert(std::derived_from<LoaderT, AssetLoader<T>>);
        if (_shuttingDown) RADRAY_ABORT("Cannot register an asset loader after shutdown");
        auto callback = EnterCallback();
        auto loader = make_unique<LoaderT>(std::forward<Args>(args)...);
        auto& registered = *loader;
        AddLoader(typeid(T), std::move(loader));
        return registered;
    }
    template <typename T>
    bool RegisterMeta(AssetMeta meta) { return AddMeta(typeid(T), std::move(meta)); }
    Nullable<const AssetMeta*> FindMeta(AssetId id) const noexcept;
    std::optional<UntypedAssetHandle> FindLoaded(AssetId id) const noexcept;

    // 具名预留在工厂返回前不可查询或提升；失败释放依赖并撤销预留。
    template <typename T, typename Factory>
    AssetRef<T> EmplaceWithHandle(std::span<const UntypedAssetHandle> deps, AssetId id, Factory&& factory) {
        const auto target = Reserve(id, typeid(T));
        if (!target.IsValid()) return {};
        auto rollback = MakeScopeGuard([&]() noexcept { Recycle(target); });
        for (const auto dep : deps) {
            if (GetState(dep) != AssetState::Ready) return {};
            AddDependency(target, dep);
        }
        const AssetHandle<T> handle{target};
        auto callback = EnterCallback();
        auto value = std::forward<Factory>(factory)(handle);
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
    AssetRef<T> Request(AssetId id) {
        const auto target = RequestSlot(id, typeid(T));
        return target.IsValid() ? AssetRef<T>{*this, AssetHandle<T>{target}} : AssetRef<T>{};
    }
    template <typename T>
    task<AssetRef<T>> LoadAsync(AssetId id) {
        const auto stop = co_await CurrentStopToken();
        if (stop.stop_requested()) co_await StopCurrentTask();
        auto ref = Request<T>(id);
        if (ref.GetState() == AssetState::Loading) co_await WaitFor(ref.Untyped(), stop, false);
        if (ref.IsReady()) co_return std::move(ref);
        co_return AssetRef<T>{};
    }
    template <typename T>
    AssetRef<T> Acquire(AssetHandle<T> handle) noexcept {
        if (_shuttingDown || !Matches(handle.Untyped(), typeid(T))) return {};
        Retain(handle.Untyped());
        return AssetRef<T>{*this, handle};
    }
    template <typename T>
    Nullable<const T*> Get(AssetHandle<T> handle) const noexcept {
        return static_cast<const T*>(Lookup(handle.Untyped(), typeid(T), false).Get());
    }
    template <typename T>
    Nullable<const T*> Resolve(AssetHandle<T> handle) const noexcept {
        return static_cast<const T*>(Lookup(handle.Untyped(), typeid(T), true).Get());
    }
    AssetState GetState(UntypedAssetHandle handle) const noexcept;
    uint32_t GetRefCount(UntypedAssetHandle handle) const noexcept;
    // 运行到当前没有可推进的任务；外部事件仍可能持有挂起任务。
    void Pump();
    bool HasPendingWork() const noexcept;
    size_t CollectGarbage() noexcept;
    // 从 owner 线程顶层关闭加载。已有强引用和相应借用仍然有效。
    void Shutdown() noexcept;

    // 外部事件通过此同步调用派发或取消 loader 的续体。
    template <typename Fn>
    decltype(auto) DispatchExternal(Fn&& dispatch) {
        auto callback = EnterCallback();
        return std::forward<Fn>(dispatch)();
    }

private:
    friend class AssetLoadContext;
    template <typename T>
    friend class AssetRef;
    class Storage;
    class SlotAwaiter;

    void AddLoader(std::type_index type, unique_ptr<detail::ErasedAssetLoader> loader);
    bool AddMeta(std::type_index type, AssetMeta meta);
    bool Matches(UntypedAssetHandle target, std::type_index type) const noexcept;
    Nullable<const void*> Lookup(UntypedAssetHandle target, std::type_index type, bool fallback) const noexcept;
    UntypedAssetHandle Reserve(AssetId id, std::type_index type);
    UntypedAssetHandle RequestSlot(AssetId id, std::type_index type);
    void Retain(UntypedAssetHandle target) noexcept;
    void Release(UntypedAssetHandle target) noexcept;
    void QueueIfUnused(UntypedAssetHandle target) noexcept;
    void ReleaseDependencies(UntypedAssetHandle target) noexcept;
    void Recycle(UntypedAssetHandle target) noexcept;
    void Publish(UntypedAssetHandle target, detail::ErasedAsset value) noexcept;
    void FailSlot(UntypedAssetHandle target) noexcept;
    void AddDependency(UntypedAssetHandle parent, UntypedAssetHandle child);
    UntypedAssetHandle RequestDependency(UntypedAssetHandle parent, AssetId id, std::optional<std::type_index> type);
    UntypedAssetHandle FindDependency(UntypedAssetHandle parent, AssetId id, std::type_index type) const noexcept;
    AssetState DependencyState(UntypedAssetHandle parent) const noexcept;
    task<void> WaitFor(UntypedAssetHandle target, stop_token stop, bool dependencies);
    task<void> RunSlotLoad(UntypedAssetHandle target);

    unique_ptr<Storage> _storage;
    TaskScope _loadScope;
    mutable size_t _callbackDepth{};
    bool _shuttingDown{};
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
    AssetState GetState() const noexcept { return _manager ? _manager->GetState(Untyped()) : AssetState::Unloaded; }
    bool IsReady() const noexcept { return GetState() == AssetState::Ready; }
    Nullable<const T*> Get() const noexcept { return _manager ? _manager->Get(_handle) : nullptr; }
    Nullable<const T*> Resolve() const noexcept { return _manager ? _manager->Resolve(_handle) : nullptr; }

private:
    friend class AssetManager;
    AssetRef(AssetManager& manager, AssetHandle<T> handle) noexcept : _manager(&manager), _handle(handle) {}
    Nullable<AssetManager*> _manager{nullptr};
    AssetHandle<T> _handle{};
};

class AssetLoadContext final {
public:
    AssetLoadContext(const AssetLoadContext&) = delete;
    AssetLoadContext& operator=(const AssetLoadContext&) = delete;
    const AssetMeta& GetMeta() const noexcept { return _meta; }
    stop_token GetStopToken() const noexcept { return _manager._loadScope.GetStopToken(); }
    template <typename T>
    AssetHandle<T> GetTargetHandle() const noexcept {
        return _manager.Matches(_target, typeid(T)) ? AssetHandle<T>{_target} : AssetHandle<T>{};
    }
    template <typename T>
    task<AssetHandle<T>> LoadDependencyAsync(AssetId id) {
        const auto child = _manager.RequestDependency(_target, id, typeid(T));
        if (child.IsValid()) {
            co_await _manager.WaitFor(child, GetStopToken(), false);
            if (_manager.GetState(child) == AssetState::Ready) co_return AssetHandle<T>{child};
        }
        _dependencyFailed = true;
        co_return AssetHandle<T>{};
    }
    template <typename T>
    Nullable<const T*> GetDependency(AssetId id) const noexcept {
        return _manager.Get(AssetHandle<T>{_manager.FindDependency(_target, id, typeid(T))});
    }

private:
    friend class AssetManager;
    AssetLoadContext(AssetManager& manager, UntypedAssetHandle target, const AssetMeta& meta) noexcept
        : _manager(manager), _target(target), _meta(meta) {}
    AssetManager& _manager;
    UntypedAssetHandle _target;
    const AssetMeta& _meta;
    bool _dependencyFailed{};
};

template <typename T>
task<detail::ErasedAsset> AssetLoader<T>::Read(AssetLoadContext& context) {
    auto result = co_await AwaitWithStopToken(LoadAsync(context), context.GetStopToken());
    if (!result || !*result) co_return detail::ErasedAsset{nullptr, nullptr};
    co_return detail::OwnAsset(std::move(**result));
}

}  // namespace radray
