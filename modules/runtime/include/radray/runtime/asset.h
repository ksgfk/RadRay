#pragma once

#include <radray/runtime_type.h>
#include <radray/types.h>

namespace radray {

class AssetManager;

/// 资产的持久标识。落盘/去重缓存的 key,跨进程有效。
using AssetId = Guid;

/// 资产基类。
///
/// 【生命周期只由引用计数决定】没有强制卸载。引用归零后资产立即销毁 (对齐到
/// AssetManager::Pump 这一确定时刻), 不做闲置缓存。由此资产可以放心向外交出指向自身
/// 内部的指针 —— 持有一份 StreamingAssetRef 即保证它们不悬垂。
///
/// 改这条不变量之前先读 docs/adr/0007-asset-lifetime-refcount-only.md。
class Asset {
public:
    Asset() noexcept = default;
    Asset(const Asset&) = delete;
    Asset(Asset&&) = delete;
    Asset& operator=(const Asset&) = delete;
    Asset& operator=(Asset&&) = delete;
    virtual ~Asset() noexcept = default;

    /// 引用归零、本资产即将析构之前调用一次 (由 AssetManager::Pump 驱动)。
    ///
    /// 【职责是"交出需要延迟销毁的数据", 不是"释放资源"】纯 CPU 数据交给析构函数, 那才是
    /// 唯一正确的地方。这里只处理不能立刻析构的东西 —— GPU 对象可能仍被上一帧录进命令
    /// 列表, 须经 manager.DeferDestroy 整包交出。不放进析构函数是因为析构里拿不到 manager。
    virtual void OnUnload(AssetManager& manager) = 0;

    /// 返回资产自身的运行时类型 id。
    virtual RuntimeTypeId GetTypeId() const noexcept = 0;

    const AssetId& GetAssetId() const noexcept { return _id; }

private:
    friend class AssetManager;

    AssetId _id;
};

template <>
struct RuntimeTypeTrait<Asset> {
    static constexpr RuntimeTypeId value{0x8b445298, 0x4242, 0x4524, 0xb3, 0x7f, 0x37, 0x24, 0xc3, 0x5b, 0x3c, 0x94};
    using Bases = std::tuple<>;
};

}  // namespace radray
