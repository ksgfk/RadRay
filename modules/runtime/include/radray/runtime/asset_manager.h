#pragma once

#include <concepts>
#include <utility>

#include <radray/types.h>
#include <radray/nullable.h>
#include <radray/coroutine.h>
#include <radray/runtime/asset.h>
#include <radray/runtime/asset_source.h>
#include <radray/runtime/service_registry.h>

// 资产槽位、引用类型与加载调度。引用语义、销毁时机与关停顺序: docs/architecture/asset-system.md

namespace radray {

class AssetManager;
class AssetWaitAwaitable;
class IWaitFrameProcessor;
class StreamingAssetRefAny;
template <class T>
requires std::derived_from<T, Asset>
class StreamingAssetRef;

/// 一个资产的槽位。定义在 asset_manager.cpp —— 本头文件只需要它的地址。
///
/// 【刻意保持不完整, 且在命名空间作用域】槽位归 AssetManager 所有, 引用只是指向它;
/// 嵌进引用类型会把所有权说反, 也会让非友元 (如 AssetWaitRecord) 无法命名它。
struct AssetSlot;

/// 资产 slot 的生命周期状态。
/// 【没有 Unloaded】只要还有一个引用指向 slot, slot 就一定存在, 故"已卸载"没有观察者。
enum class AssetState {
    Loading,   ///< 空位已占,加载协程在飞,Object 尚未就绪。
    Ready,     ///< 资产已构造,可访问。
    Faulted,   ///< 加载失败。
    Canceled,  ///< 加载被取消。
};

struct AssetWaitRecord : ManualCoroutineRecord {
    /// 等待目标。由等待者持有的 ref 保住, 故在记录存活期内有效。
    /// 【只用于比较, 不解引用】AssetSlot 在此是不完整类型。
    const AssetSlot* Slot{nullptr};
};

/// AssetManager 的加载请求。具体 loader 的参数形状完全由调用方决定;
/// AssetManager 只消费统一的 task<AssetLoadResult> 结果。
struct AssetLoadRequest {
    AssetId Id;
    task<AssetLoadResult> Task;
    string DebugName{};
};

/// 【类型擦除的 streaming 引用】同时表达加载状态与 ready 后的资产访问。
///
/// 表示是 manager + slot 裸指针 (地址稳定 + RefCount > 0 保活), 故 Get() 无需查表 ——
/// 它在渲染热路径上 (PSO 缓存、SceneProxy)。
///
/// 【单线程】拷贝/移动/析构/状态查询/资产访问都只能在拥有 AssetManager 的线程进行。
/// 资产内部数据被各系统怎么跨线程使用不属资产系统管辖, 但引用本身不跨线程。
/// 【必须全部死在 AssetManager 之前】slot 随 manager 释放, 之后连 IsValid() 都答不出来。
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

    /// 引用非空。Loading / Ready / Faulted / Canceled 都是 valid; 默认构造与 Reset 后 invalid。
    bool IsValid() const noexcept;
    /// 终态:Ready / Faulted / Canceled 任一。
    bool IsCompleted() const noexcept;
    bool IsReady() const noexcept;
    bool IsCompletedSuccessfully() const noexcept { return IsReady(); }
    bool IsFaulted() const noexcept;
    bool IsCanceled() const noexcept;
    explicit operator bool() const noexcept { return IsReady(); }

    void Cancel() const noexcept;

    const AssetId& GetAssetId() const noexcept;
    RuntimeTypeId GetTypeId() const noexcept;

    /// 等待本引用离开 Loading 态。`co_await ref` 得到 bool: true = 已到终态,
    /// false = 等待者自己被取消 (不是资产加载失败)。
    AssetWaitAwaitable operator co_await() const noexcept;

    /// 【比较是否指向同一个槽位】这是 id 去重的可观测形式。刻意不比较 AssetId ——
    /// 两个无效引用的 id 都是空, 那样会相等。
    bool operator==(const StreamingAssetRefAny& other) const noexcept {
        return _manager == other._manager && _slot == other._slot;
    }

    template <class T>
    requires std::derived_from<T, Asset>
    bool Is() const noexcept;

    template <class T>
    requires std::derived_from<T, Asset>
    StreamingAssetRef<T> CastTo() const noexcept;

    void Reset() noexcept;

private:
    friend class AssetManager;
    friend class AssetWaitAwaitable;
    template <class U>
    requires std::derived_from<U, Asset>
    friend class StreamingAssetRef;

    StreamingAssetRefAny(AssetManager* manager, AssetSlot* slot) noexcept;

    /// Ready 之后最终实例的类型描述符, 否则 nullptr。Is<T>/CastTo<T> 是模板, 而 AssetSlot
    /// 在本头文件里是不完整类型, 故类型判定要经这个非模板出口取到描述符。
    const RuntimeTypeInfo* GetTypeInfo() const noexcept;

    AssetManager* _manager{nullptr};
    AssetSlot* _slot{nullptr};
};

/// 类型安全 streaming 引用。本质是 StreamingAssetRefAny + 类型视图:
/// - Loading 时可查询状态,但 Get()/operator bool 仍为空。
/// - Ready 且最终实例 is-a T 时可直接访问资产。
/// - 参与引用计数;最后一份引用消失后资产在下一次 AssetManager::Pump 被销毁。
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

    const AssetId& GetAssetId() const noexcept { return _ref.GetAssetId(); }
    RuntimeTypeId GetTypeId() const noexcept { return _ref.GetTypeId(); }

    /// 见 StreamingAssetRefAny::operator co_await。
    AssetWaitAwaitable operator co_await() const noexcept;

    /// 见 StreamingAssetRefAny::operator==。
    bool operator==(const StreamingAssetRef& other) const noexcept { return _ref == other._ref; }
    bool operator==(const StreamingAssetRefAny& other) const noexcept { return _ref == other; }

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

/// co_await 一份 streaming 引用的 awaitable。恢复点在 AssetManager::Pump 把加载结果提交、
/// 槽位进入终态之后。await_resume 返回 false 表示【等待者自己】被取消, 而非资产加载失败。
///
/// 【必须持有 ref 的副本】等待期间它是槽位的一个引用持有者, 否则槽位会因"外部都放手了"
/// 而在 Pump 里被回收, 令等待记录指向已销毁的 slot。
class AssetWaitAwaitable {
public:
    explicit AssetWaitAwaitable(StreamingAssetRefAny ref) noexcept : _ref(std::move(ref)) {}

    bool await_ready() const noexcept { return !_ref.IsValid() || _ref.IsCompleted(); }

    /// 【模板化以拿到 promise】取消所需的 stop token 只能从 promise 的 env 里取, 而
    /// coroutine_handle<> 已把它擦除。见 GetCoroutineStopToken。
    template <class Promise>
    bool await_suspend(std::coroutine_handle<Promise> continuation) {
        return Suspend(continuation, GetCoroutineStopToken(continuation));
    }

    bool await_resume() noexcept;

private:
    bool Suspend(std::coroutine_handle<> continuation, stop_token stop);

    StreamingAssetRefAny _ref;
    AssetWaitRecord* _record{nullptr};
    /// 挂起前就已被取消。此时没有记录, 但结论是"没等到"而非"已到终态"。
    bool _canceledBeforeSuspend{false};
};

inline AssetWaitAwaitable StreamingAssetRefAny::operator co_await() const noexcept {
    return AssetWaitAwaitable{*this};
}

template <class T>
requires std::derived_from<T, Asset>
AssetWaitAwaitable StreamingAssetRef<T>::operator co_await() const noexcept {
    return AssetWaitAwaitable{AsAny()};
}

/// 资产仓库。按 AssetId 去重的单表 + 引用计数。
///
/// - 单线程使用, 不加锁 (协程推进、表操作、引用增减全在主线程)。
/// - Load 只接受已创建好的 task<AssetLoadResult>, 再包装为内部 task<void> 提交给 TaskScope。
/// - slot 自己维护 per-load stop_source 与 pending result; TaskScope 只负责结构化生命周期。
/// - 【引用计数是唯一的回收权威】没有 Unload / CollectUnreferenced / 闲置缓存。最后一份
///   引用消失后, 资产在下一次 Pump 里 OnUnload + 析构 + 摘除 slot。
///
class AssetManager {
public:
    AssetManager() noexcept;
    AssetManager(const AssetManager&) = delete;
    AssetManager(AssetManager&&) = delete;
    AssetManager& operator=(const AssetManager&) = delete;
    AssetManager& operator=(AssetManager&&) = delete;

    /// 【存活引用必须先于本对象消失】关停顺序是 World → RenderSystem → AssetManager →
    /// GpuSystem。违反时记 error log 并照样卸载, 不 abort (关停期 abort 会掩盖真正的首因)。
    ~AssetManager() noexcept;

    /// 异步发起加载。按 id 去重:命中在飞或已就绪 slot 直接复用。
    StreamingAssetRefAny Load(AssetLoadRequest request);

    /// 经可选 IAssetSource 按持久 id 发起加载。来源未装配或 id 未登记时记错误并返回空引用。
    StreamingAssetRefAny Load(const AssetId& id);

    /// 类型化加载入口。T 只是返回引用的类型视图,最终实例类型由 loader 的结果决定。
    template <class T>
    requires std::derived_from<T, Asset>
    StreamingAssetRef<T> Load(AssetLoadRequest request);

    template <class T>
    requires std::derived_from<T, Asset>
    StreamingAssetRef<T> Load(const AssetId& id);

    /// 人类可读路径入口。路径由 IAssetSource 解析为持久 id，再进入同一 slot 表。
    template <class T>
    requires std::derived_from<T, Asset>
    StreamingAssetRef<T> Load(std::string_view relPath);

    /// 等待 streaming 引用离开 Loading 状态。等待者取消不会取消底层资产加载。
    /// 【薄转发】直接 `co_await ref` 等价; 本函数额外把"等待者被取消"转成对当前 task
    /// 的 stop 传播。
    task<void> Wait(StreamingAssetRefAny ref);

    template <class T>
    requires std::derived_from<T, Asset>
    task<void> Wait(StreamingAssetRef<T> ref);

    template <class T>
    requires std::derived_from<T, Asset>
    task<StreamingAssetRef<T>> LoadAndWait(AssetLoadRequest request);

    /// 不启动 task,仅按 id 去重并登记一个 ready object。主要给测试/工具使用。
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

    /// 提交加载协程写入的 pending result, 并销毁引用已归零的资产。
    void Pump();

    /// 资产内部数据的延迟销毁入口。由 Asset::OnUnload 调用。
    ///
    /// 【整包交出, 不逐个交】payload 是一个可移动的可调用对象 (通常是捕获了整组 GPU 对象
    /// 的 lambda), 在一个帧边界之后被销毁。销毁顺序由 payload 内部的捕获/成员声明顺序
    /// 显式表达, 不要依赖多次调用的先后。理由见
    /// docs/adr/0009-deferred-destroy-hands-over-suspension.md。
    ///
    /// 同一次 Pump 内的全部 payload 攒成一批共用一个协程帧。
    /// wait processor 未装配时立即销毁 payload 并记 error log。
    template <class F>
    void DeferDestroy(F&& payload) {
        EnqueueDeferred(make_unique<DeferredPayloadImpl<std::decay_t<F>>>(std::forward<F>(payload)));
    }

    /// 注入帧边界等待器 (非拥有)。【必须装配】见 ServiceTraits<AssetManager>。
    /// 调用方保证它活得比本 AssetManager 更久 (关停顺序里 AssetManager 先于 GpuSystem 死)。
    void SetWaitFrameProcessor(IWaitFrameProcessor* processor) noexcept { _waitFrame = processor; }

    /// 注入可选资产来源（非拥有）。来源必须活过本对象及其全部在飞加载。
    void SetAssetSource(Nullable<IAssetSource*> source) noexcept { _assetSource = source; }

    uint32_t GetAssetCount() const noexcept;

private:
    friend class AssetWaitAwaitable;
    friend class StreamingAssetRefAny;

    using Slot = AssetSlot;

    /// 延迟销毁 payload 的类型擦除包装。析构即销毁被捕获的数据。
    struct DeferredPayload {
        virtual ~DeferredPayload() noexcept = default;
    };

    template <class F>
    struct DeferredPayloadImpl final : DeferredPayload {
        explicit DeferredPayloadImpl(F&& f) noexcept(std::is_nothrow_move_constructible_v<F>)
            : Value(std::move(f)) {}
        explicit DeferredPayloadImpl(const F& f) : Value(f) {}
        F Value;
    };

    Slot* FindSlot(const AssetId& id) const noexcept;
    Slot* EmplaceLoadingSlot(const AssetId& id);
    StreamingAssetRefAny MakeRef(Slot* slot) noexcept;
    StreamingAssetRefAny LoadSourcePath(std::string_view relPath);

    void PumpLoadResults();
    void FlushDeferredBatch();

    task<void> RunLoad(StreamingAssetRefAny ref, task<AssetLoadResult> loadTask);
    void StoreLoadResult(Slot* slot, AssetLoadResult result) noexcept;
    void StoreLoadCanceled(Slot* slot) noexcept;
    void CommitLoadResult(Slot* slot, AssetLoadResult result) noexcept;
    void ResumeWaiters(Slot* slot) noexcept;
    void CollectZeroRefSlots();
    void DestroySlot(Slot* slot) noexcept;

    AssetWaitRecord* RegisterWait(Slot* slot, stop_token stop, std::coroutine_handle<> continuation);

    void EnqueueDeferred(unique_ptr<DeferredPayload> payload);
    task<void> RunDeferredDestroy(vector<unique_ptr<DeferredPayload>> batch);

    static void AddRef(Slot* slot) noexcept;
    static void Release(Slot* slot) noexcept;

    IWaitFrameProcessor* _waitFrame{nullptr};
    Nullable<IAssetSource*> _assetSource{nullptr};
    TaskScope _loadScope;
    ManualCoroutineScheduler<AssetWaitRecord> _waiters;
    unordered_map<AssetId, unique_ptr<Slot>> _slots;
    /// 在飞加载的 slot。manager 自持一份引用 —— 加载期间外部引用可能全部消失, 但槽位要
    /// 活到协程跑完。
    vector<StreamingAssetRefAny> _activeLoads;
    /// 本帧待延迟销毁的 payload。Pump 时整批交给一个协程。
    vector<unique_ptr<DeferredPayload>> _pendingDeferred;
    /// 递归保护: CollectZeroRefSlots 里销毁资产会放开它持有的引用, 从而令更多 slot 归零。
    bool _collecting{false};
};

template <class T>
requires std::derived_from<T, Asset>
StreamingAssetRef<T> AssetManager::Load(AssetLoadRequest request) {
    return Load(std::move(request)).template CastTo<T>();
}

template <class T>
requires std::derived_from<T, Asset>
StreamingAssetRef<T> AssetManager::Load(const AssetId& id) {
    return Load(id).template CastTo<T>();
}

template <class T>
requires std::derived_from<T, Asset>
StreamingAssetRef<T> AssetManager::Load(std::string_view relPath) {
    return LoadSourcePath(relPath).template CastTo<T>();
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

template <class T>
requires std::derived_from<T, Asset>
bool StreamingAssetRefAny::Is() const noexcept {
    const RuntimeTypeInfo* typeInfo = GetTypeInfo();
    return typeInfo != nullptr && typeInfo->IsA(runtime_type_id_v<T>);
}

template <class T>
requires std::derived_from<T, Asset>
StreamingAssetRef<T> StreamingAssetRefAny::CastTo() const noexcept {
    if (_slot == nullptr) {
        return StreamingAssetRef<T>{};
    }
    // Ready 之前最终类型未知, 故不能拒绝 —— 那正是 streaming 引用要表达的"还没到"。
    // Ready 之后类型不符则返回空引用: 调用方要的视图与实际实例无关。
    if (IsReady() && !Is<T>()) {
        return StreamingAssetRef<T>{};
    }
    return StreamingAssetRef<T>{*this};
}

/// 依赖声明(非侵入,类外特化):AssetManager 只需要 IWaitFrameProcessor 接口,
/// 由 ServiceRegistry 通过 RuntimeTypeTrait 的 Bases 别名解析到具体实现(如 GpuSystem)。
template <>
struct ServiceTraits<AssetManager> {
    static constexpr auto Inject = std::tuple{&AssetManager::SetWaitFrameProcessor};
};

template <>
struct RuntimeTypeTrait<AssetManager> {
    static constexpr RuntimeTypeId value{0xd4f18ebe, 0xb5c4, 0x46c2, 0x8b, 0x7b, 0x2d, 0xde, 0x5c, 0x96, 0xe5, 0xcf};
    using Bases = std::tuple<>;
};

}  // namespace radray
