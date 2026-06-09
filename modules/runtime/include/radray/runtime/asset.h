#pragma once

#include <radray/types.h>

namespace radray {

/// 资产的持久标识。落盘/去重缓存的 key,跨进程有效。
/// 当前用路径字符串占位,后续可替换为 GUID。
using AssetId = string;

/// 运行时类型标识。用于类型擦除后的向下转换校验(AssetRefAny -> AssetRef<T>)。
/// 不依赖 RTTI。仅在单个二进制内稳定(本项目 runtime 为静态库,满足)。
/// 【限制】为扁平精确匹配:只能转换为 Load 时的确切类型,不支持转为基类。
struct AssetTypeId {
    uint64_t Value{0};

    constexpr bool IsValid() const noexcept { return Value != 0; }
    constexpr static AssetTypeId Invalid() noexcept { return {}; }
    constexpr auto operator<=>(const AssetTypeId&) const noexcept = default;
};

/// 类型 -> AssetTypeId 的特化点(trait)。
/// 默认实现用“函数内静态变量地址”生成每类型唯一且进程内稳定的 id,
/// 无需任何派生类样板。若某类型需要固定 id(如跨模块),可显式特化本 trait。
template <class T>
struct AssetTypeTrait {
    static AssetTypeId Id() noexcept {
        static const char marker = 0;
        return AssetTypeId{reinterpret_cast<uint64_t>(&marker)};
    }
};

/// 获取类型 T 的运行时 id。
template <class T>
inline AssetTypeId GetAssetTypeId() noexcept {
    return AssetTypeTrait<T>::Id();
}

/// 资产在 AssetManager slot 中的生命周期状态。
enum class AssetState {
    /// 已创建 slot,CPU 数据正在加载(同步加载下基本不可见)。
    Loading,
    /// CPU 数据就绪(GPU 资源是否就绪由派生类自己描述)。
    Loaded,
    /// 加载失败,slot 仍占位以便上层查询错误。
    Failed,
    /// 引用计数已归零,等待 AssetManager::CollectGarbage 真正回收。
    PendingRelease,
    /// slot 已被回收,处于空闲待复用状态(未持有任何 Asset)。
    Free,
};

/// 所有资产的多态基类。对应 UE5 的 UObject 资产(如 UStaticMesh)在本项目中的最小化等价物。
///
/// 设计要点:
/// - 基类【不区分 CPU/GPU】。参考 UE5:UObject/UStreamableRenderAsset 基类不知道
///   CPU/GPU 之分,具体资产(如 UStaticMesh)自己持有 CPU 源数据与渲染数据,
///   自己决定上传/丢弃/释放时机。本 Asset 同理:派生类自行持有并管理
///   CPU 源数据和 GPU 资源,基类只提供生命周期钩子。
/// - Asset 本身不持有引用计数。计数由 AssetManager 的 slot 维护,Asset 不知道自己被谁引用。
/// - GPU 资源"何时安全销毁"(是否需要等待渲染同步)是派生类与 render 层的实现细节,
///   不属于 AssetManager 的关注点。
class Asset {
public:
    Asset() noexcept = default;
    Asset(const Asset&) = delete;
    Asset(Asset&&) = delete;
    Asset& operator=(const Asset&) = delete;
    Asset& operator=(Asset&&) = delete;
    virtual ~Asset() noexcept = default;

    /// 资产被加载进 AssetManager 后调用一次(对应 UStaticMesh 加载完成)。
    /// 派生类在此准备 CPU 源数据、按需上传 GPU 资源等。不带 CPU/GPU 语义。
    virtual void OnLoad() = 0;

    /// 引用归零、CollectGarbage 即将销毁前调用一次(对应 BeginDestroy → ReleaseResources)。
    /// 派生类在此释放 GPU 资源、丢弃 CPU 数据等。调用发生在 CollectGarbage 的可控时机。
    virtual void OnUnload() = 0;

    const AssetId& GetAssetId() const noexcept { return _id; }

private:
    friend class AssetManager;

    AssetId _id;
};

}  // namespace radray
