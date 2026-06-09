#pragma once

#include <atomic>
#include <concepts>
#include <limits>
#include <mutex>
#include <utility>

#include <radray/types.h>
#include <radray/nullable.h>
#include <radray/runtime/asset.h>

namespace radray {

class AssetManager;
class AssetRefAny;
template <class T>
requires std::derived_from<T, Asset>
class AssetRef;

/// 运行时弱句柄。POD,可自由拷贝/比较,不参与引用计数。
///
/// - Index/Generation 是 AssetManager slot 表的坐标,带 generation 以检测悬垂:
///   slot 被回收复用后 generation 自增,旧句柄解析时即失效(而非访问到野数据)。
/// - 仅在运行时有效,进程重启后 Index 无意义,因此【不可序列化】。
///   需要持久化时序列化 AssetId,运行时再 Load 换回句柄。
struct AssetHandle {
    uint32_t Index{std::numeric_limits<uint32_t>::max()};
    uint32_t Generation{0};

    constexpr bool IsValid() const noexcept { return Index != std::numeric_limits<uint32_t>::max(); }

    constexpr static AssetHandle Invalid() noexcept { return {}; }

    constexpr auto operator<=>(const AssetHandle&) const noexcept = default;
};

/// 【类型擦除的强引用】。承载全部 RAII + 计数逻辑,Get() 返回基类 Asset*。
///
/// - 与 AssetRef<T> 共享同一套计数机制:AssetRef<T> 内部就是一个 AssetRefAny。
/// - 适用于“只需持有引用保活、不关心具体类型”的场景(通用依赖表、编辑器引用可视化等)。
/// - 拷贝 +1、析构 -1,语义与 AssetRef<T> 完全一致。
/// - 向下转换为 AssetRef<T> 走 CastTo<T>(),由 slot 存的 AssetTypeId 校验。
class AssetRefAny {
public:
    AssetRefAny() noexcept = default;
    AssetRefAny(std::nullptr_t) noexcept {}

    AssetRefAny(const AssetRefAny& other) noexcept;
    AssetRefAny(AssetRefAny&& other) noexcept;
    AssetRefAny& operator=(const AssetRefAny& other) noexcept;
    AssetRefAny& operator=(AssetRefAny&& other) noexcept;
    ~AssetRefAny() noexcept;

    /// 解析为基类指针。句柄失效返回 nullptr。
    Asset* Get() const noexcept;
    Asset* operator->() const noexcept { return Get(); }
    Asset& operator*() const noexcept { return *Get(); }

    bool IsValid() const noexcept { return Get() != nullptr; }
    explicit operator bool() const noexcept { return IsValid(); }

    /// 降级为不参与计数的弱句柄。
    AssetHandle GetHandle() const noexcept { return _handle; }

    /// 查询底层资产的运行时类型 id。失效返回 Invalid。
    AssetTypeId GetTypeId() const noexcept;

    /// 运行时类型判定:底层资产是否为 Load 时的确切类型 T。
    template <class T>
    requires std::derived_from<T, Asset>
    bool Is() const noexcept;

    /// 向下转换为类型安全引用。类型不匹配返回空引用。
    /// 【限制】只能转为 Load 时的确切类型,不支持转为中间基类。
    template <class T>
    requires std::derived_from<T, Asset>
    AssetRef<T> CastTo() const noexcept;

    void Reset() noexcept;

private:
    friend class AssetManager;
    template <class U>
    requires std::derived_from<U, Asset>
    friend class AssetRef;

    /// adopt 语义:不增计数(调用方保证句柄已带一个待接管的引用)。
    AssetRefAny(AssetManager* manager, AssetHandle handle) noexcept
        : _manager(manager), _handle(handle) {}

    AssetManager* _manager{nullptr};
    AssetHandle _handle{AssetHandle::Invalid()};
};

/// 运行时【类型安全】强引用。本质是 AssetRefAny + 编译期类型外衣。
///
/// - 计数与生命周期逻辑全部委托给内部 AssetRefAny,拷贝/移动/析构均可默认。
/// - Get() 在 AssetRefAny::Get() 基础上多一步 static_cast<T*>。
/// - 可隐式退化为 AssetRefAny(拷贝/移动均保持计数正确)。
/// - 组件应持有 AssetRef<T> 而非裸句柄,以表达“我正在使用这份资产”。
template <class T>
requires std::derived_from<T, Asset>
class AssetRef {
public:
    AssetRef() noexcept = default;
    AssetRef(std::nullptr_t) noexcept {}

    /// 解析底层资产指针。句柄失效返回 nullptr。
    T* Get() const noexcept { return static_cast<T*>(_ref.Get()); }
    T* operator->() const noexcept { return Get(); }
    T& operator*() const noexcept { return *Get(); }

    bool IsValid() const noexcept { return _ref.IsValid(); }
    explicit operator bool() const noexcept { return IsValid(); }

    /// 降级为不参与计数的弱句柄。
    AssetHandle GetHandle() const noexcept { return _ref.GetHandle(); }

    /// 擦除类型,退化为 AssetRefAny(保持计数)。
    const AssetRefAny& AsAny() const& noexcept { return _ref; }
    operator AssetRefAny() const& noexcept { return _ref; }
    operator AssetRefAny() && noexcept { return std::move(_ref); }

    void Reset() noexcept { _ref.Reset(); }

private:
    friend class AssetManager;
    friend class AssetRefAny;

    /// adopt 语义:直接接管一个已带引用的句柄(manager 内部使用)。
    AssetRef(AssetManager* manager, AssetHandle handle) noexcept
        : _ref(manager, handle) {}
    /// 从一个 AssetRefAny 拷贝构造(+1 计数),供 CastTo 使用。
    explicit AssetRef(AssetRefAny ref) noexcept : _ref(std::move(ref)) {}

    AssetRefAny _ref;
};

/// 资产仓库。唯一所有权 + 手写引用计数 + 归零延迟回收。
///
/// 关注点【仅】引用计数与去重:
/// - 按 AssetId 去重:同一资产只加载一份,多处共享同一 slot。
/// - slot 持有 unique_ptr<Asset> 唯一所有权;引用计数集中在 slot 上(非侵入 Asset)。
/// - AssetRef 归零 → slot 标记 PendingRelease;CollectGarbage() 时统一回收。
/// - 不依赖帧号/fence。GPU 资源"何时安全销毁"由 Asset 派生类与 render 层自行保证。
///
/// 与 UE5 的本质区别:UE 的 manager 不拥有 asset 内存,只持 GC root 影响回收时机,
/// 真正的销毁交给 GC。本 AssetManager 则【直接完全持有】每个 Asset 的生命周期
/// (slot 内 unique_ptr),引用计数归零本身就是销毁触发点,不存在独立的回收器。
class AssetManager {
public:
    AssetManager() noexcept = default;
    AssetManager(const AssetManager&) = delete;
    AssetManager(AssetManager&&) = delete;
    AssetManager& operator=(const AssetManager&) = delete;
    AssetManager& operator=(AssetManager&&) = delete;
    ~AssetManager() noexcept;

    /// 按 id 加载资产并返回强引用。
    /// - 命中已存在(含 PendingRelease 待回收)的 slot 时直接复活共享,不重复加载。
    /// - 未命中则构造 T、调用 Asset::OnLoad、登记入 slot 表。
    /// T 必须派生自 Asset。
    template <class T, class... Args>
    requires std::derived_from<T, Asset>
    AssetRef<T> Load(const AssetId& id, Args&&... args) {
        const AssetTypeId typeId = GetAssetTypeId<T>();
        {
            std::scoped_lock lk(_mutex);
            AssetHandle existing = AcquireExistingLocked(id);
            if (existing.IsValid()) {
                return AssetRef<T>{this, existing};
            }
        }
        // 在锁外构造并 OnLoad,避免持锁期间执行可能耗时的加载/上传。
        unique_ptr<Asset> object = make_unique<T>(std::forward<Args>(args)...);
        object->_id = id;
        object->OnLoad();
        std::scoped_lock lk(_mutex);
        // 双重检查:构造期间可能有其他线程已插入同一 id。
        AssetHandle existing = AcquireExistingLocked(id);
        if (existing.IsValid()) {
            object->OnUnload();
            return AssetRef<T>{this, existing};
        }
        AssetHandle handle = InsertLocked(std::move(object), id, typeId);
        return AssetRef<T>{this, handle};
    }

    /// 查询已加载资产,不发起加载。未命中返回空引用。
    /// 命中 PendingRelease 的 slot 会触发复活(同 Lock)。
    /// 仅是 FindAny 之上的类型安全包装。
    template <class T>
    requires std::derived_from<T, Asset>
    AssetRef<T> Find(const AssetId& id) noexcept {
        return FindAny(id).template CastTo<T>();
    }

    /// 将弱句柄升级为强引用(+1 计数)。句柄失效则返回空引用。
    /// 【复活语义】若 slot 处于 PendingRelease(计数已归零但尚未被
    /// CollectGarbage 销毁),Lock 将其计数从 0 加回、状态改回 Loaded、
    /// 移出 pending 列表,资产被“救活”而非重新加载。
    /// 这与 Release/CollectGarbage 对同一 slot 的访问必须互斥(见 CollectGarbage)。
    /// 仅是 LockAny 之上的类型安全包装。
    template <class T>
    requires std::derived_from<T, Asset>
    AssetRef<T> Lock(AssetHandle handle) noexcept {
        return LockAny(handle).template CastTo<T>();
    }

    /// 【类型擦除版】不需要知道具体类型即可升级弱句柄为强引用。
    AssetRefAny LockAny(AssetHandle handle) noexcept {
        std::scoped_lock lk(_mutex);
        return AssetRefAny{this, LockLocked(handle)};
    }

    /// 【类型擦除版】按 id 查询已加载资产,不发起加载。
    AssetRefAny FindAny(const AssetId& id) noexcept {
        std::scoped_lock lk(_mutex);
        return AssetRefAny{this, AcquireExistingLocked(id)};
    }

    /// 回收引用计数已归零的 slot:调用 Asset::OnUnload,
    /// 销毁 Asset,generation 自增,slot 回收并清理 id 索引。
    /// 由上层在合适的节点(如帧末)调用。
    ///
    /// 【复活复检】销毁前必须重新检查 StrongRefs == 0。Release 归零
    /// 与本函数销毁之间存在时间窗口,期间 Lock/Find 可能已把计数加回。
    /// 若发现 StrongRefs > 0,说明中途复活,则跳过销毁、移出 pending。
    /// 因此仅凭 atomic 计数不足以覆盖“读计数 + 改状态 + 移出列表”这组
    /// 复合操作,单线程访问天然安全,多线程需 manager 级锁或 slot 状态 CAS。
    void CollectGarbage();

    /// 当前存活(含 PendingRelease)的资产数量。
    uint32_t GetAssetCount() const noexcept;

private:
    template <class T>
    requires std::derived_from<T, Asset>
    friend class AssetRef;
    friend class AssetRefAny;

    struct Slot {
        uint32_t Generation{0};
        std::atomic<int32_t> StrongRefs{0};
        unique_ptr<Asset> Object;
        AssetTypeId TypeId{AssetTypeId::Invalid()};
        AssetState State{AssetState::Free};
    };

    // AssetRefAny 计数操作入口(non-public,仅供引用类型调用)。
    // 强引用钉住 slot,AddRef/Resolve 无需加锁(slot 地址稳定、generation 必然匹配)。
    void AddRef(AssetHandle handle) noexcept;
    void Release(AssetHandle handle) noexcept;
    Nullable<Asset*> Resolve(AssetHandle handle) const noexcept;
    AssetTypeId ResolveTypeId(AssetHandle handle) const noexcept;

    // 以下 helper 均要求调用方已持有 _mutex。
    /// 命中已存在 slot 则 +1 计数(必要时复活)并返回句柄,否则返回 Invalid。
    AssetHandle AcquireExistingLocked(const AssetId& id) noexcept;
    /// 插入全新资产(复用 free slot 或新增),StrongRefs=1、state=Loaded。
    AssetHandle InsertLocked(unique_ptr<Asset> object, const AssetId& id, AssetTypeId typeId) noexcept;
    /// 弱句柄升级为强引用的加锁实现。
    AssetHandle LockLocked(AssetHandle handle) noexcept;

    mutable std::mutex _mutex;
    vector<unique_ptr<Slot>> _slots;        // 原地稳定地址,避免搬动 atomic
    vector<uint32_t> _freeSlots;            // 空闲 slot 下标
    vector<uint32_t> _pending;              // 待回收 slot 下标
    unordered_map<AssetId, uint32_t> _idIndex;  // id 去重索引
    uint32_t _aliveCount{0};                // 存活(非 Free) slot 数
};

// ===== AssetRefAny 实现(需 AssetManager 完整定义后) =====

inline AssetRefAny::AssetRefAny(const AssetRefAny& other) noexcept
    : _manager(other._manager), _handle(other._handle) {
    if (_manager != nullptr && _handle.IsValid()) {
        _manager->AddRef(_handle);
    }
}

inline AssetRefAny::AssetRefAny(AssetRefAny&& other) noexcept
    : _manager(other._manager), _handle(other._handle) {
    other._manager = nullptr;
    other._handle = AssetHandle::Invalid();
}

inline AssetRefAny& AssetRefAny::operator=(const AssetRefAny& other) noexcept {
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

inline AssetRefAny& AssetRefAny::operator=(AssetRefAny&& other) noexcept {
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

inline AssetRefAny::~AssetRefAny() noexcept {
    Reset();
}

inline void AssetRefAny::Reset() noexcept {
    if (_manager != nullptr && _handle.IsValid()) {
        _manager->Release(_handle);
    }
    _manager = nullptr;
    _handle = AssetHandle::Invalid();
}

inline Asset* AssetRefAny::Get() const noexcept {
    if (_manager == nullptr || !_handle.IsValid()) {
        return nullptr;
    }
    return _manager->Resolve(_handle).Get();
}

inline AssetTypeId AssetRefAny::GetTypeId() const noexcept {
    if (_manager == nullptr || !_handle.IsValid()) {
        return AssetTypeId::Invalid();
    }
    return _manager->ResolveTypeId(_handle);
}

template <class T>
requires std::derived_from<T, Asset>
bool AssetRefAny::Is() const noexcept {
    return IsValid() && GetTypeId() == GetAssetTypeId<T>();
}

template <class T>
requires std::derived_from<T, Asset>
AssetRef<T> AssetRefAny::CastTo() const noexcept {
    if (!Is<T>()) {
        return AssetRef<T>{};
    }
    return AssetRef<T>{*this};  // 拷贝构造 +1 计数
}

}  // namespace radray
