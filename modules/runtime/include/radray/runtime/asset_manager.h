#pragma once

#include <concepts>
#include <utility>

#include <radray/types.h>
#include <radray/nullable.h>
#include <radray/coroutine.h>
#include <radray/runtime/asset.h>
#include <radray/runtime/service_registry.h>

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
/// 【为何在命名空间作用域而不是嵌套进 StreamingAssetRefAny】: 槽位归 AssetManager 所有
/// (_slots 那张表持有它们), 引用只是指向它。嵌进引用类型会把所有权关系说反, 而且会让
/// 任何非友元 (如 AssetWaitRecord) 都无法命名它 —— 嵌套类无法从外部前向声明, 于是
/// 只能退化成 void*。
///
/// 【刻意保持不完整】: 外部能拿到这个名字但做不了任何事; 唯一的构造入口
/// (StreamingAssetRefAny 的私有构造) 也不对外开放。
struct AssetSlot;

/// 资产 slot 的生命周期状态。
///
/// 【没有 Unloaded】: 引用计数是资产生命周期的唯一权威 (见 asset.h), 故只要还有一个
/// StreamingAssetRef 指向 slot, slot 就一定存在 —— "已卸载"这个状态没有观察者。
/// 从前它存在是因为 Unload 能无视引用计数销毁槽位。
enum class AssetState {
    Loading,   ///< 空位已占,加载协程在飞,Object 尚未就绪。
    Ready,     ///< 资产已构造,可访问。
    Faulted,   ///< 加载失败。
    Canceled,  ///< 加载被取消。
};

struct AssetWaitRecord : ManualCoroutineRecord {
    /// 等待目标。slot 由本记录的等待者所持有的 ref 保住, 故这个指针在记录存活期内有效。
    ///
    /// 【只用于比较, 不解引用】: AssetSlot 在此是不完整类型, 本记录也无需知道它的内容 ——
    /// ResumeWaiters 拿它与目标 slot 做相等判断即可。
    const AssetSlot* Slot{nullptr};
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

/// AssetManager 的加载请求。具体 loader 的参数形状完全由调用方决定;
/// AssetManager 只消费统一的 task<AssetLoadResult> 结果。
struct AssetLoadRequest {
    AssetId Id;
    task<AssetLoadResult> Task;
    string DebugName{};
};

/// 【类型擦除的 streaming 引用】。同时表达加载状态与 ready 后的资产访问。
///
/// 表示是 manager + slot 裸指针: slot 是 unordered_map 里的 unique_ptr 元素, 地址稳定,
/// 而 RefCount > 0 保证它不被销毁 —— 故不需要 generation 校验, 也不需要每次访问都按 id
/// 查一次哈希表 (Get() 在渲染热路径上: PSO 缓存、SceneProxy)。
///
/// 【单线程】: 拷贝 / 移动 / 析构 / 状态查询 / 资产访问全部只能在拥有 AssetManager 的线程
/// (主线程) 进行 —— RefCount 是普通整数, 且增减要触碰 manager 的表。资产【内部数据】被
/// 各系统怎么跨线程使用不属资产系统管辖, 但引用本身不跨线程。
///
/// 【必须全部死在 AssetManager 之前】: slot 随 manager 一同释放, 之后 _slot 悬垂。
/// manager 析构时若仍有存活引用会记 error log 并照样卸载, 见 AssetManager 的析构说明。
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

    /// 【指向同一个槽位】。这是 id 去重的可观测形式: Load 命中既有 slot 时返回的引用与
    /// 原引用相等。刻意【不】比较 AssetId —— 两个无效引用的 id 都是空, 那样会相等。
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
/// 【持有 ref 的副本, 这是必须的】: 等待期间它是槽位的一个引用持有者, 于是槽位不会因
/// "外部都放手了"而在 Pump 里被回收 —— 那会让等待记录指向一个已销毁的 slot。
class AssetWaitAwaitable {
public:
    explicit AssetWaitAwaitable(StreamingAssetRefAny ref) noexcept : _ref(std::move(ref)) {}

    bool await_ready() const noexcept { return !_ref.IsValid() || _ref.IsCompleted(); }

    /// 【模板化以拿到 promise】: 取消所需的 stop token 只能从 promise 的 env 里取, 而
    /// coroutine_handle<> 已经把它擦除了。见 GetCoroutineStopToken。
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
/// - 单线程使用,不加锁(协程推进、表操作、引用增减全在主线程)。
/// - Load 只接受已经创建好的 task<AssetLoadResult>,再包装为内部 task<void> 提交给 TaskScope。
/// - slot 自己维护 per-load stop_source 与 pending result;TaskScope 只负责结构化生命周期。
/// - 【引用计数是唯一的回收权威】: 没有 Unload, 没有 CollectUnreferenced, 也没有闲置缓存。
///   最后一份引用消失后, 资产在下一次 Pump 里 OnUnload + 析构 + 摘除 slot。
///
/// == 为何销毁对齐到 Pump 而不是就地发生在引用归零处 ==
///
/// 就地销毁会让 ~StreamingAssetRefAny (一条 noexcept 路径) 跑任意代码: 资产析构会放开它
/// 自己持有的 StreamingAssetRef 成员, 从而递归销毁别的 slot; 而遍历资产表时放开一个引用
/// 会当场令迭代器失效。对齐到 Pump 后两个问题一并消失, 且"资产销毁只在一个确定时刻发生"
/// 本身便于推理。这仍然是"归零即销毁", 不是保留策略 —— 只是动作对齐到帧内一个固定点。
class AssetManager {
public:
    AssetManager() noexcept;
    AssetManager(const AssetManager&) = delete;
    AssetManager(AssetManager&&) = delete;
    AssetManager& operator=(const AssetManager&) = delete;
    AssetManager& operator=(AssetManager&&) = delete;

    /// 【存活引用必须先于本对象消失】: slot 存储随本对象释放, 之后任何 StreamingAssetRef
    /// 的 _slot 都是悬垂指针 —— 连 IsValid() 都答不出来。参照 Application::Shutdown 的
    /// 顺序: World → RenderSystem → AssetManager → GpuSystem。
    ///
    /// 违反时【记 error log 并照样卸载】, 不 abort。理由是此刻两种结果都坏, 而 GPU 资源
    /// 必须在 device 之前交出 —— 泄漏比悬垂更难查, 且关停期 abort 会掩盖真正的首因
    /// (通常是某个 system 忘了 reset)。详见析构实现处。
    ~AssetManager() noexcept;

    /// 异步发起加载。按 id 去重:命中在飞或已就绪 slot 直接复用。
    StreamingAssetRefAny Load(AssetLoadRequest request);

    /// 类型化加载入口。T 只是返回引用的类型视图,最终实例类型由 loader 的结果决定。
    template <class T>
    requires std::derived_from<T, Asset>
    StreamingAssetRef<T> Load(AssetLoadRequest request);

    /// 等待 streaming 引用离开 Loading 状态。等待者取消不会取消底层资产加载。
    ///
    /// 【薄转发】: 真正的实现是 StreamingAssetRefAny::operator co_await, 直接 `co_await ref`
    /// 等价。本函数保留是因为它额外把"等待者被取消"转成对当前 task 的 stop 传播。
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
    /// 【整包交出, 不逐个交】: payload 是一个可移动的可调用对象 (通常是捕获了整组 GPU
    /// 对象的 lambda)。它在一个帧边界之后被销毁, 销毁顺序由 payload 内部的捕获/成员声明
    /// 顺序显式表达 —— 例如 TextureAsset 必须让 view 先于 texture 死, 那是 lambda 里
    /// 两个 unique_ptr 的声明顺序问题, 而不是"交出去的先后能不能被队列保持"的问题。
    ///
    /// 【为何不是"交出对象"而是"包住对象"】: 逐对象交出的接口 (从前的
    /// IRenderResourceRecycler::RecycleRenderResource) 把销毁顺序寄托在队列语义上, 那是
    /// 一条无法在类型上表达、也无法在 review 中看见的隐式契约。
    ///
    /// 【一帧一个协程帧】: 同一次 Pump 内的全部 payload 攒成一批, 共用一个等待帧边界的
    /// 协程。故大量资产同时归零不会产生大量协程。
    ///
    /// wait processor 未装配时【立即销毁 payload】并记 error log —— 见实现处说明。
    template <class F>
    void DeferDestroy(F&& payload) {
        EnqueueDeferred(make_unique<DeferredPayloadImpl<std::decay_t<F>>>(std::forward<F>(payload)));
    }

    /// 注入帧边界等待器(非拥有)。【必须装配】,见 ServiceTraits<AssetManager>。
    ///
    /// 【生命周期】:调用方保证它活得比本 AssetManager 更久 (见 Application::Shutdown 的
    /// 关停顺序: AssetManager 先于 GpuSystem 销毁)。
    void SetWaitFrameProcessor(IWaitFrameProcessor* processor) noexcept { _waitFrame = processor; }

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
    TaskScope _loadScope;
    ManualCoroutineScheduler<AssetWaitRecord> _waiters;
    unordered_map<AssetId, unique_ptr<Slot>> _slots;
    /// 在飞加载的 slot。manager 自持一份引用 —— 加载期间外部引用可能全部消失, 但槽位要
    /// 活到协程跑完 (见 Load 的说明)。
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
