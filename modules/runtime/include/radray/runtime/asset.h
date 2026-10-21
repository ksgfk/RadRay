#pragma once

// 资产生命周期与身份: docs/architecture/asset-system.md

#include <filesystem>
#include <string_view>

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
class Asset {
public:
    Asset() noexcept = default;
    Asset(const Asset&) = delete;
    Asset(Asset&&) = delete;
    Asset& operator=(const Asset&) = delete;
    Asset& operator=(Asset&&) = delete;
    virtual ~Asset() noexcept = default;

    /// Called on GT after the last asset use, including every accepted GPU work use, is released.
    /// Release views before their backing storage; raw RHI users provide their own completion owner.
    virtual void OnUnload(AssetManager& manager) = 0;

    const AssetId& GetAssetId() const noexcept { return _id; }

private:
    friend class AssetManager;

    AssetId _id;
};

template <>
struct RuntimeTypeTrait<Asset> {
    static constexpr RuntimeTypeId value{0x8b445298, 0x4242, 0x4524, 0xb3, 0x7f, 0x37, 0x24, 0xc3, 0x5b, 0x3c, 0x94};
};

/// 文件路径派生的 AssetId。同一份文件必须得到同一个 id, 不同文件必须得到不同 id。
///
/// namespacePrefix 做资产类型的命名空间隔离 ("shader" / "image" / ...), 各资产类型必须
/// 用独占前缀。路径先归一化再哈希, 这是【正确性要求而非优化】。
AssetId MakeAssetIdFromPath(std::string_view namespacePrefix, const std::filesystem::path& path);

}  // namespace radray
