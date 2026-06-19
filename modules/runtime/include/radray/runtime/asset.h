#pragma once

#include <radray/runtime_type.h>
#include <radray/runtime/render_resource_recycler.h>

namespace radray {

/// 资产的持久标识。落盘/去重缓存的 key,跨进程有效。
using AssetId = Guid;

/// 运行时类型标识。用于类型擦除后的向下转换校验(StreamingAssetRefAny -> StreamingAssetRef<T>)。
/// 不依赖 RTTI,由资产类型手填固定 Guid,跨进程/跨模块稳定。
/// 【限制】为扁平精确匹配:只能转换为 Load 时的确切类型,不支持转为基类。
using AssetTypeId = RuntimeTypeId;

/// 所有资产的多态基类。对应 UE5 的 UObject 资产(如 UStaticMesh)在本项目中的最小化等价物。
///
/// 设计要点:
/// - 基类【不区分 CPU/GPU】。具体资产(如 StaticMesh)自行持有 CPU 源数据与 GPU 资源,
///   自己决定上传/丢弃/释放时机。基类只提供生命周期钩子。
/// - 资产采用【构造函数一次性初始化】:Asset 在被放入 AssetManager 时已经是完整、可用状态
///   (CPU 数据 + GPU 资源都已就绪)。加载/上传发生在构造之前,由加载协程完成。
/// - Asset 本身不持有引用计数。AssetManager 不做引用计数与垃圾回收;资产生命周期由
///   应用层显式 Unload 控制。StreamingAssetRef/StreamingAssetRefAny 均为【非拥有】弱句柄。
class Asset {
public:
    Asset() noexcept = default;
    Asset(const Asset&) = delete;
    Asset(Asset&&) = delete;
    Asset& operator=(const Asset&) = delete;
    Asset& operator=(Asset&&) = delete;
    virtual ~Asset() noexcept = default;

    /// 资产被回收(Unload)或 AssetManager 析构前调用一次。派生类在此释放 GPU 资源、丢弃 CPU 数据等。
    virtual void OnUnload(IRenderResourceRecycler& recycler) = 0;

    /// 返回资产自身的运行时类型 id。
    virtual AssetTypeId GetTypeId() const noexcept = 0;

    const AssetId& GetAssetId() const noexcept { return _id; }

private:
    friend class AssetManager;

    AssetId _id;
};

}  // namespace radray
