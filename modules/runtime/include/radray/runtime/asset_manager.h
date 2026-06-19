#pragma once

#include <optional>

#include <radray/types.h>
#include <radray/nullable.h>
#include <radray/sparse_set.h>
#include <radray/coroutine.h>
#include <radray/runtime/asset.h>

namespace radray {

class AssetManager;
class StreamingAssetRefAny;
template <class T>
requires std::derived_from<T, Asset>
class StreamingAssetRef;

/// 运行时弱句柄。POD,可自由拷贝/比较。指向 AssetManager 的 slot。
/// 带 generation 检测悬垂:slot 回收复用后旧句柄解析即失效。【不可序列化】。
using AssetHandle = SparseSetHandle;

/// 资产 slot 的生命周期状态。
enum class AssetState {
    Loading,   ///< 空位已占,加载协程在飞,Object 尚未就绪。
    Ready,     ///< 资产已构造,可 Resolve。
    Faulted,   ///< 加载失败。
    Canceled,  ///< 加载被取消。
};

struct AssetLoadResult {
    unique_ptr<Asset> Object;
    string Error;
    bool Succeeded{false};

    static AssetLoadResult Success(unique_ptr<Asset> object) noexcept {
        AssetLoadResult result;
        result.Object = std::move(object);
        result.Succeeded = true;
        return result;
    }

    static AssetLoadResult Failure(string error = {}) noexcept {
        AssetLoadResult result;
        result.Error = std::move(error);
        return result;
    }

    bool IsSuccess() const noexcept { return Succeeded && Object != nullptr; }
};

using AssetLoadTask = task<AssetLoadResult>;

/// AssetManager 的加载请求。具体 loader 的参数形状完全由调用方决定；
/// AssetManager 只消费统一的 task<AssetLoadResult> 结果。ExpectedTypeId 通常由 Load<T> 填充。
struct AssetLoadRequest {
    AssetId Id;
    AssetTypeId ExpectedTypeId{Guid::Empty()};
    AssetLoadTask Task;
    string DebugName{};
};

/// 【类型擦除的 streaming 弱引用】。同时表达加载状态与 ready 后的资产访问。
/// Get() 仅在 Ready 且对象仍存活时返回基类 Asset*;Loading/Faulted/Canceled/Unload 均返回 nullptr。
class StreamingAssetRefAny {
public:
    StreamingAssetRefAny() noexcept = default;
    StreamingAssetRefAny(std::nullptr_t) noexcept {}

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
    AssetTypeId GetExpectedTypeId() const noexcept;

    template <class T>
    requires std::derived_from<T, Asset>
    bool Is() const noexcept;

    template <class T>
    requires std::derived_from<T, Asset>
    StreamingAssetRef<T> CastTo() const noexcept;

    void Reset() noexcept {
        _manager = nullptr;
        _handle = AssetHandle::Invalid();
        _id = AssetId{};
    }

private:
    friend class AssetManager;
    template <class U>
    requires std::derived_from<U, Asset>
    friend class StreamingAssetRef;

    StreamingAssetRefAny(AssetManager* manager, AssetHandle handle, AssetId id) noexcept
        : _manager(manager), _handle(handle), _id(id) {}

    AssetManager* _manager{nullptr};
    AssetHandle _handle{AssetHandle::Invalid()};
    AssetId _id{};
};

/// 类型安全 streaming 弱引用。本质是 manager + handle + 期望类型:
/// - Loading 时可查询状态,但 Get()/operator bool 仍为空。
/// - Ready 且类型匹配时可直接访问资产。
/// - 不参与引用计数;显式 Unload 后自动失效。
template <class T>
requires std::derived_from<T, Asset>
class StreamingAssetRef {
public:
    StreamingAssetRef() noexcept = default;
    StreamingAssetRef(std::nullptr_t) noexcept {}

    T* Get() const noexcept {
        Asset* asset = _ref.Get();
        if (asset == nullptr || asset->GetTypeId() != runtime_type_id_v<T>) {
            return nullptr;
        }
        return static_cast<T*>(asset);
    }
    T* operator->() const noexcept { return Get(); }
    T& operator*() const noexcept { return *Get(); }

    bool IsValid() const noexcept { return _ref.IsValid(); }
    bool IsCompleted() const noexcept { return _ref.IsCompleted(); }
    bool IsReady() const noexcept { return Get() != nullptr; }
    bool IsCompletedSuccessfully() const noexcept { return IsReady(); }
    bool IsFaulted() const noexcept { return _ref.IsFaulted(); }
    bool IsCanceled() const noexcept { return _ref.IsCanceled(); }
    explicit operator bool() const noexcept { return IsReady(); }

    void Cancel() const noexcept { _ref.Cancel(); }

    AssetHandle GetHandle() const noexcept { return _ref.GetHandle(); }
    const AssetId& GetAssetId() const noexcept { return _ref.GetAssetId(); }
    AssetTypeId GetTypeId() const noexcept { return _ref.GetTypeId(); }
    AssetTypeId GetExpectedTypeId() const noexcept { return _ref.GetExpectedTypeId(); }

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

/// 资产仓库。用内部协程承接异步加载结果,单表空位模型,无引用计数,无 GC。
///
/// - 单线程使用,不加锁(协程推进、表操作、run 钩子全在主/泵线程)。
/// - Load 只接受已经创建好的 AssetLoadTask,再包装为内部 task<void> 提交给 TaskScope。
/// - slot 自己维护 per-load stop_source 与 pending result；TaskScope 只负责结构化生命周期。
/// - 资产回收由应用层显式 Unload 控制(AssetManager 不做引用计数 / 不做 GC)。
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

    /// 类型化加载入口。只填充期望资产类型,不关心 loader 的参数形状。
    template <class T>
    requires std::derived_from<T, Asset>
    StreamingAssetRef<T> Load(AssetLoadRequest request);

    /// 不启动 task，仅按 id 去重并登记一个 ready object。主要给测试/工具使用。
    StreamingAssetRefAny AddReady(const AssetId& id, unique_ptr<Asset> object, AssetTypeId expectedTypeId = Guid::Empty());

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

    /// 应用层显式回收资产(唯一回收入口)。命中在飞 slot 则先取消、延迟到终态再回收。
    void Unload(const AssetId& id) noexcept;

    /// 注入 GPU 资源回收器。未设置时退回立即析构,保持无 GPUSystem 测试场景的旧行为。
    void SetRenderResourceRecycler(IRenderResourceRecycler* recycler) noexcept { _recycler = recycler; }

    /// 提交加载协程写入的 pending result。
    void Pump();

    uint32_t GetAssetCount() const noexcept { return _slots.Count(); }

private:
    friend class StreamingAssetRefAny;

    struct AssetSlot {
        AssetId Id;
        AssetTypeId TypeId;
        AssetState State{AssetState::Loading};
        unique_ptr<Asset> Object;
        stop_source Stop;
        std::optional<AssetLoadResult> PendingResult;
        bool PendingCanceled{false};
        bool PendingUnload{false};
    };

    AssetHandle FindHandle(const AssetId& id) const noexcept;
    AssetHandle EmplaceLoadingSlot(const AssetId& id, AssetTypeId typeId);
    task<void> RunLoad(AssetHandle handle, AssetLoadTask loadTask);
    void StoreLoadResult(AssetHandle handle, AssetLoadResult result) noexcept;
    void StoreLoadCanceled(AssetHandle handle) noexcept;
    void OnLoadComplete(AssetHandle handle, AssetLoadResult result) noexcept;
    void OnLoadStopped(AssetHandle handle) noexcept;
    void FinalizeTerminalSlot(AssetHandle handle);
    void DestroySlot(AssetHandle handle) noexcept;
    IRenderResourceRecycler& GetRecycler() noexcept;

    Nullable<Asset*> ResolveAsset(AssetHandle handle) const noexcept;
    AssetTypeId ResolveTypeId(AssetHandle handle) const noexcept;
    AssetTypeId ResolveExpectedTypeId(AssetHandle handle) const noexcept;
    AssetState ResolveState(AssetHandle handle) const noexcept;
    bool IsSlotAlive(AssetHandle handle) const noexcept;

    TaskScope _loadScope;
    SparseSet<unique_ptr<AssetSlot>> _slots;
    unordered_map<AssetId, AssetHandle> _idIndex;
    vector<AssetHandle> _activeLoads;
    IRenderResourceRecycler* _recycler{nullptr};
};

template <class T>
requires std::derived_from<T, Asset>
bool StreamingAssetRefAny::Is() const noexcept {
    if (_manager == nullptr || !_handle.IsValid() || !_manager->IsSlotAlive(_handle)) {
        return false;
    }
    AssetTypeId expected = _manager->ResolveExpectedTypeId(_handle);
    if (!expected.IsEmpty()) {
        return expected == runtime_type_id_v<T>;
    }
    Asset* asset = Get();
    return asset != nullptr && asset->GetTypeId() == runtime_type_id_v<T>;
}

template <class T>
requires std::derived_from<T, Asset>
StreamingAssetRef<T> StreamingAssetRefAny::CastTo() const noexcept {
    if (_manager == nullptr || !_handle.IsValid() || !_manager->IsSlotAlive(_handle)) {
        return StreamingAssetRef<T>{};
    }
    AssetTypeId expected = _manager->ResolveExpectedTypeId(_handle);
    if (!expected.IsEmpty() && expected != runtime_type_id_v<T>) {
        return StreamingAssetRef<T>{};
    }
    Asset* asset = Get();
    if (asset != nullptr && asset->GetTypeId() != runtime_type_id_v<T>) {
        return StreamingAssetRef<T>{};
    }
    return StreamingAssetRef<T>{*this};
}

template <class T>
requires std::derived_from<T, Asset>
StreamingAssetRef<T> AssetManager::Load(AssetLoadRequest request) {
    request.ExpectedTypeId = runtime_type_id_v<T>;
    return Load(std::move(request)).template CastTo<T>();
}

template <class T>
requires std::derived_from<T, Asset>
StreamingAssetRef<T> AssetManager::AddReady(const AssetId& id, unique_ptr<T> object) {
    unique_ptr<Asset> asset = std::move(object);
    return AddReady(id, std::move(asset), runtime_type_id_v<T>).template CastTo<T>();
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

}  // namespace radray
