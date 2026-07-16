#pragma once

#include <atomic>
#include <optional>
#include <span>

#include <radray/types.h>
#include <radray/nullable.h>
#include <radray/sparse_set.h>
#include <radray/coroutine.h>
#include <radray/runtime/asset.h>
#include <radray/runtime/service_registry.h>

namespace radray {

class AssetManager;
class AssetWaitAwaitable;
class StreamingAssetRefAny;
template <class T>
requires std::derived_from<T, Asset>
class StreamingAssetRef;

/// 资产 slot 的生命周期状态。
enum class AssetState {
    Loading,   ///< 空位已占,加载协程在飞,Object 尚未就绪。
    Ready,     ///< 资产已构造,可 Resolve。
    Faulted,   ///< 加载失败。
    Canceled,  ///< 加载被取消。
    Unloaded,  ///< slot 已回收或 AssetManager 已析构。
};

/// 运行时弱句柄。POD,可自由拷贝/比较。指向 AssetManager 的 slot。
/// 带 generation 检测悬垂:slot 回收复用后旧句柄解析即失效。【不可序列化】。
using AssetHandle = SparseSetHandle;

struct AssetWaitRecord : ManualCoroutineRecord {
    AssetHandle Handle{AssetHandle::Invalid()};
};

struct AssetLoadResult {
    unique_ptr<Asset> Object;
    const RuntimeTypeInfo* TypeInfo{nullptr};
    string Error;
    bool Succeeded{false};

    static AssetLoadResult Success(unique_ptr<Asset> object, const RuntimeTypeInfo& typeInfo) noexcept {
        AssetLoadResult result;
        result.Object = std::move(object);
        result.TypeInfo = &typeInfo;
        result.Succeeded = true;
        return result;
    }

    template <class T>
    requires std::derived_from<T, Asset> && (!std::same_as<T, Asset>)
    static AssetLoadResult Success(unique_ptr<T> object) noexcept {
        unique_ptr<Asset> asset = std::move(object);
        return Success(std::move(asset), runtime_type_info_v<T>);
    }

    static AssetLoadResult Failure(string error = {}) noexcept {
        AssetLoadResult result;
        result.Error = std::move(error);
        return result;
    }

    bool IsSuccess() const noexcept { return Succeeded && Object != nullptr && TypeInfo != nullptr; }
};

using AssetLoadTask = task<AssetLoadResult>;

/// 引用控制块。由 slot 与所有 StreamingAssetRef(Any) 共享(shared_ptr)。
/// 生命周期独立于 slot,用于跨线程引用计数和只读状态查询。TypeInfo 在最终实例构造完成后写入,
/// 再由 Ready 状态的 release/acquire 发布;其中 Id 始终是最终实例的精确类型 id。
struct AssetRefControl {
    std::atomic<uint32_t> RefCount{0};
    std::atomic<AssetState> State{AssetState::Loading};
    const RuntimeTypeInfo* TypeInfo{nullptr};

    AssetState LoadState() const noexcept {
        return State.load(std::memory_order_acquire);
    }

    void PublishState(AssetState state) noexcept {
        State.store(state, std::memory_order_release);
    }
};

/// AssetManager 的加载请求。具体 loader 的参数形状完全由调用方决定；
/// AssetManager 只消费统一的 task<AssetLoadResult> 结果。
struct AssetLoadRequest {
    AssetId Id;
    AssetLoadTask Task;
    string DebugName{};
};

/// 【类型擦除的 streaming 引用】。同时表达加载状态与 ready 后的资产访问。
/// Get() 仅在 Ready 且对象仍存活时返回基类 Asset*;Loading/Faulted/Canceled/Unload 均返回 nullptr。
/// 构造/拷贝会对目标 slot 增加引用计数,析构/移动出/Reset 则减少;引用计数归零的资产
/// 可由 AssetManager::CollectUnreferenced 统一回收。
///
/// 【线程安全】不同引用实例可在任意线程拷贝/移动/析构/Reset,也可查询状态与类型
/// (IsValid/IsCompleted/IsReady/IsFaulted/IsCanceled/GetTypeId/Is/CastTo)。
/// 同一个引用实例不能与 Reset/赋值并发使用。资产访问 Get/Cancel/Wait 以及 AssetManager 的 slot 表操作
/// 仍只能在拥有 AssetManager 的线程(主/泵线程)进行。
class StreamingAssetRefAny {
public:
    StreamingAssetRefAny() noexcept = default;
    StreamingAssetRefAny(std::nullptr_t) noexcept {}
    StreamingAssetRefAny(const StreamingAssetRefAny& other) noexcept;
    StreamingAssetRefAny(StreamingAssetRefAny&& other) noexcept;
    StreamingAssetRefAny& operator=(const StreamingAssetRefAny& other) noexcept;
    StreamingAssetRefAny& operator=(StreamingAssetRefAny&& other) noexcept;
    ~StreamingAssetRefAny() noexcept;

    Asset* Get() const noexcept;
    Asset* operator->() const noexcept { return Get(); }
    Asset& operator*() const noexcept { return *Get(); }

    /// slot 仍然存在。Loading/Faulted/Canceled 都是 valid；Unload 后 invalid。
    bool IsValid() const noexcept;
    /// 终态:Ready / Faulted / Canceled 任一。
    bool IsCompleted() const noexcept;
    bool IsReady() const noexcept;
    bool IsCompletedSuccessfully() const noexcept { return IsReady(); }
    bool IsFaulted() const noexcept;
    bool IsCanceled() const noexcept;
    explicit operator bool() const noexcept { return IsReady(); }

    void Cancel() const noexcept;

    AssetHandle GetHandle() const noexcept { return _handle; }
    const AssetId& GetAssetId() const noexcept { return _id; }
    AssetTypeId GetTypeId() const noexcept;

    template <class T>
    requires std::derived_from<T, Asset>
    bool Is() const noexcept;

    template <class T>
    requires std::derived_from<T, Asset>
    StreamingAssetRef<T> CastTo() const noexcept;

    void Reset() noexcept;

private:
    friend class AssetManager;
    template <class U>
    requires std::derived_from<U, Asset>
    friend class StreamingAssetRef;

    StreamingAssetRefAny(AssetManager* manager, AssetHandle handle, AssetId id, shared_ptr<AssetRefControl> control) noexcept;

    AssetManager* _manager{nullptr};
    AssetHandle _handle{AssetHandle::Invalid()};
    AssetId _id{};
    shared_ptr<AssetRefControl> _control{};
};

/// 类型安全 streaming 引用。本质是 manager + handle + 类型视图:
/// - Loading 时可查询状态,但 Get()/operator bool 仍为空。
/// - Ready 且最终实例 is-a T 时可直接访问资产。
/// - 参与引用计数(透过底层 StreamingAssetRefAny);显式 Unload 或引用归零回收后自动失效。
template <class T>
requires std::derived_from<T, Asset>
class StreamingAssetRef {
public:
    StreamingAssetRef() noexcept = default;
    StreamingAssetRef(std::nullptr_t) noexcept {}

    T* Get() const noexcept {
        Asset* asset = _ref.Get();
        if (asset == nullptr || !_ref.template Is<T>()) {
            return nullptr;
        }
        return static_cast<T*>(asset);
    }
    T* operator->() const noexcept { return Get(); }
    T& operator*() const noexcept { return *Get(); }

    bool IsValid() const noexcept { return _ref.IsValid(); }
    bool IsCompleted() const noexcept { return _ref.IsCompleted(); }
    bool IsReady() const noexcept { return _ref.IsReady() && _ref.template Is<T>(); }
    bool IsCompletedSuccessfully() const noexcept { return IsReady(); }
    bool IsFaulted() const noexcept { return _ref.IsFaulted(); }
    bool IsCanceled() const noexcept { return _ref.IsCanceled(); }
    explicit operator bool() const noexcept { return IsReady(); }

    void Cancel() const noexcept { _ref.Cancel(); }

    AssetHandle GetHandle() const noexcept { return _ref.GetHandle(); }
    const AssetId& GetAssetId() const noexcept { return _ref.GetAssetId(); }
    AssetTypeId GetTypeId() const noexcept { return _ref.GetTypeId(); }

    const StreamingAssetRefAny& AsAny() const& noexcept { return _ref; }
    operator StreamingAssetRefAny() const& noexcept { return _ref; }
    operator StreamingAssetRefAny() && noexcept { return std::move(_ref); }

    void Reset() noexcept { _ref.Reset(); }

private:
    friend class AssetManager;
    friend class StreamingAssetRefAny;

    explicit StreamingAssetRef(StreamingAssetRefAny ref) noexcept : _ref(std::move(ref)) {}

    StreamingAssetRefAny _ref;
};

/// 资产仓库。用内部协程承接异步加载结果,单表空位模型,带引用计数。
///
/// - 单线程使用,不加锁(协程推进、表操作、run 钩子全在主/泵线程)。
/// - Load 只接受已经创建好的 AssetLoadTask,再包装为内部 task<void> 提交给 TaskScope。
/// - slot 自己维护 per-load stop_source 与 pending result；TaskScope 只负责结构化生命周期。
/// - 每个 slot 维护引用计数:StreamingAssetRef(Any) 构造/拷贝 +1,析构/Reset -1。
/// - 资产回收有两条路径:应用层显式 Unload,或 CollectUnreferenced 统一回收所有引用归零的 slot。
class AssetManager {
public:
    AssetManager() noexcept = default;
    AssetManager(const AssetManager&) = delete;
    AssetManager(AssetManager&&) = delete;
    AssetManager& operator=(const AssetManager&) = delete;
    AssetManager& operator=(AssetManager&&) = delete;
    ~AssetManager() noexcept;

    /// 异步发起加载。按 id 去重:命中在飞或已就绪 slot 直接复用。
    StreamingAssetRefAny Load(AssetLoadRequest request);

    /// 类型化加载入口。T 只是返回引用的类型视图,最终实例类型由 loader 的结果决定。
    template <class T>
    requires std::derived_from<T, Asset>
    StreamingAssetRef<T> Load(AssetLoadRequest request);

    /// 等待 streaming 引用离开 Loading 状态。等待者取消不会取消底层资产加载。
    task<void> Wait(StreamingAssetRefAny ref);

    template <class T>
    requires std::derived_from<T, Asset>
    task<void> Wait(StreamingAssetRef<T> ref);

    template <class T>
    requires std::derived_from<T, Asset>
    task<StreamingAssetRef<T>> LoadAndWait(AssetLoadRequest request);

    /// 不启动 task，仅按 id 去重并登记一个 ready object。主要给测试/工具使用。
    StreamingAssetRefAny AddReady(const AssetId& id, unique_ptr<Asset> object, const RuntimeTypeInfo& typeInfo);

    template <class T>
    requires std::derived_from<T, Asset>
    StreamingAssetRef<T> AddReady(const AssetId& id, unique_ptr<T> object);

    /// 不发起加载,按 id 查现有 slot(在飞或就绪)。未命中返回无效引用。
    StreamingAssetRefAny Find(const AssetId& id) noexcept;

    template <class T>
    requires std::derived_from<T, Asset>
    StreamingAssetRef<T> Find(const AssetId& id) noexcept;

    /// 直查资产 streaming 引用。Get()/operator bool 只在 Ready 后有效。
    template <class T>
    requires std::derived_from<T, Asset>
    StreamingAssetRef<T> Get(const AssetId& id) noexcept;

    /// 请求取消一次在飞加载。终态前生效,协程在挂起点感知后以 Canceled 终止。
    void Cancel(const StreamingAssetRefAny& ref) noexcept;

    template <class T>
    requires std::derived_from<T, Asset>
    void Cancel(const StreamingAssetRef<T>& ref) noexcept {
        Cancel(ref.AsAny());
    }

    /// 应用层显式强制回收资产。命中在飞 slot 则先取消、延迟到终态再回收。
    /// 【无视引用计数】:仍存活的 StreamingAssetRef 会静默失效(generation 保护不崩溃,
    /// 但 Get() 返回 nullptr)。DEBUG 下若回收时仍有引用会打 warning。
    /// 关卡切换 / 热重载 / 编辑器手动删除等确需强制清空的场景使用;否则优先 CollectUnreferenced。
    void Unload(const AssetId& id) noexcept;

    /// 回收所有引用计数归零的资产 slot。命中在飞 slot 则先取消、延迟到终态再回收。
    /// 返回实际回收(或标记延迟回收)的 slot 数量。
    uint32_t CollectUnreferenced() noexcept;

    /// 注入渲染资源回收器(非拥有)。未设置时 GPU 资源退回立即析构,保持无 GpuSystem 测试场景的旧行为。
    void SetRecycler(IRenderResourceRecycler* recycler) noexcept { _recycler = recycler; }

    /// 提交加载协程写入的 pending result。
    void Pump();

    uint32_t GetAssetCount() const noexcept { return _slots.Count(); }

private:
    friend class AssetWaitAwaitable;
    friend class StreamingAssetRefAny;

    struct AssetSlot {
        AssetId Id;
        AssetState State{AssetState::Loading};
        unique_ptr<Asset> Object;
        stop_source Stop;
        std::optional<AssetLoadResult> PendingResult;
        shared_ptr<AssetRefControl> Control{make_shared<AssetRefControl>()};
        bool PendingCanceled{false};
        bool PendingUnload{false};

        void SetState(AssetState state) noexcept {
            State = state;
            Control->PublishState(state);
        }
    };

    AssetHandle FindHandle(const AssetId& id) const noexcept;
    AssetHandle EmplaceLoadingSlot(const AssetId& id);
    task<void> RunLoad(AssetHandle handle, AssetLoadTask loadTask);
    void StoreLoadResult(AssetHandle handle, AssetLoadResult result) noexcept;
    void StoreLoadCanceled(AssetHandle handle) noexcept;
    void OnLoadComplete(AssetHandle handle, AssetLoadResult result) noexcept;
    void OnLoadStopped(AssetHandle handle) noexcept;
    vector<AssetWaitRecord*> TakeWaiters(AssetHandle handle) noexcept;
    void ResumeWaiters(std::span<AssetWaitRecord* const> waiters) noexcept;
    void FinalizeTerminalSlot(AssetHandle handle);
    void DestroySlot(AssetHandle handle) noexcept;
    void UnloadSlot(AssetHandle handle) noexcept;
    StreamingAssetRefAny MakeRef(AssetHandle handle, const AssetId& id) noexcept;
    IRenderResourceRecycler& GetRecycler() noexcept;

    AssetWaitRecord* RegisterWait(AssetHandle handle, stop_token stop, std::coroutine_handle<> continuation);

    Nullable<Asset*> ResolveAsset(AssetHandle handle) const noexcept;

    IRenderResourceRecycler* _recycler{nullptr};
    TaskScope _loadScope;
    ManualCoroutineScheduler<AssetWaitRecord> _waiters;
    SparseSet<unique_ptr<AssetSlot>> _slots;
    unordered_map<AssetId, AssetHandle> _idIndex;
    vector<AssetHandle> _activeLoads;
};

template <class T>
requires std::derived_from<T, Asset>
bool StreamingAssetRefAny::Is() const noexcept {
    if (_control == nullptr) {
        return false;
    }
    const AssetState state = _control->LoadState();
    if (state != AssetState::Ready) {
        return false;
    }
    const RuntimeTypeInfo* typeInfo = _control->TypeInfo;
    return typeInfo != nullptr && typeInfo->IsA(runtime_type_id_v<T>);
}

template <class T>
requires std::derived_from<T, Asset>
StreamingAssetRef<T> StreamingAssetRefAny::CastTo() const noexcept {
    if (_control == nullptr) {
        return StreamingAssetRef<T>{};
    }
    const AssetState state = _control->LoadState();
    if (state == AssetState::Unloaded) {
        return StreamingAssetRef<T>{};
    }
    if (state == AssetState::Ready) {
        const RuntimeTypeInfo* typeInfo = _control->TypeInfo;
        if (typeInfo == nullptr || !typeInfo->IsA(runtime_type_id_v<T>)) {
            return StreamingAssetRef<T>{};
        }
    }
    return StreamingAssetRef<T>{*this};
}

template <class T>
requires std::derived_from<T, Asset>
StreamingAssetRef<T> AssetManager::Load(AssetLoadRequest request) {
    return Load(std::move(request)).template CastTo<T>();
}

template <class T>
requires std::derived_from<T, Asset>
task<void> AssetManager::Wait(StreamingAssetRef<T> ref) {
    co_await Wait(ref.AsAny());
}

template <class T>
requires std::derived_from<T, Asset>
task<StreamingAssetRef<T>> AssetManager::LoadAndWait(AssetLoadRequest request) {
    StreamingAssetRef<T> ref = Load<T>(std::move(request));
    co_await Wait(ref.AsAny());
    co_return ref;
}

template <class T>
requires std::derived_from<T, Asset>
StreamingAssetRef<T> AssetManager::AddReady(const AssetId& id, unique_ptr<T> object) {
    unique_ptr<Asset> asset = std::move(object);
    return AddReady(id, std::move(asset), runtime_type_info_v<T>).template CastTo<T>();
}

template <class T>
requires std::derived_from<T, Asset>
StreamingAssetRef<T> AssetManager::Find(const AssetId& id) noexcept {
    return Find(id).template CastTo<T>();
}

template <class T>
requires std::derived_from<T, Asset>
StreamingAssetRef<T> AssetManager::Get(const AssetId& id) noexcept {
    return Find<T>(id);
}

/// 依赖声明(非侵入,类外特化):AssetManager 只需要 IRenderResourceRecycler 接口,
/// 由 ServiceRegistry 通过 RuntimeTypeTrait 的 Bases 别名解析到具体实现(如 GpuSystem)。
template <>
struct ServiceTraits<AssetManager> {
    static constexpr auto Inject = std::tuple{&AssetManager::SetRecycler};
};

template <>
struct RuntimeTypeTrait<AssetManager> {
    static constexpr RuntimeTypeId value{0xd4f18ebe, 0xb5c4, 0x46c2, 0x8b, 0x7b, 0x2d, 0xde, 0x5c, 0x96, 0xe5, 0xcf};
    using Bases = std::tuple<>;
};

}  // namespace radray
