#pragma once

#include <concepts>
#include <optional>
#include <string_view>
#include <utility>

#include <radray/coroutine.h>
#include <radray/runtime/asset.h>
#include <radray/types.h>

namespace radray {

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

    template <class T>
    requires std::derived_from<T, Asset> && (!std::same_as<T, Asset>)
    static AssetLoadResult Success(unique_ptr<T> object) noexcept {
        unique_ptr<Asset> asset = std::move(object);
        return Success(std::move(asset));
    }

    static AssetLoadResult Failure(string error = {}) noexcept {
        AssetLoadResult result;
        result.Error = std::move(error);
        return result;
    }

    bool IsSuccess() const noexcept { return Succeeded && Object != nullptr; }
};

/// AssetManager 的可选资产来源。实现必须在 CreateLoadTask 返回前同步取齐加载所需数据；
/// 返回的惰性 task 启动后不得回查本对象中的可变条目。
class IAssetSource {
public:
    virtual ~IAssetSource() noexcept = default;

    virtual std::optional<task<AssetLoadResult>> CreateLoadTask(const AssetId& id) = 0;
    virtual std::optional<AssetId> ResolveId(std::string_view relPath) const = 0;
};

}  // namespace radray
