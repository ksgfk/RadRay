#pragma once

#include <radray/sparse_set.h>

namespace radray {
class AssetManager;
class ShaderAsset;
class MaterialAsset;
// Handles belong to one manager; IsValid only checks the empty sentinel.
template <typename T>
class AssetHandle {
public:
    constexpr AssetHandle() noexcept = default;

    // 仅判断是否非空；元素是否仍存活须通过所属 AssetManager 查询。
    constexpr bool IsValid() const noexcept { return _handle.IsValid(); }
    constexpr uint32_t GetIndex() const noexcept { return _handle.Index; }
    constexpr uint32_t GetGeneration() const noexcept { return _handle.Generation; }
    constexpr auto operator<=>(const AssetHandle&) const noexcept = default;

private:
    friend class AssetManager;
    explicit constexpr AssetHandle(SparseSetHandle handle) noexcept : _handle(handle) {}

    SparseSetHandle _handle{};
};

}  // namespace radray

template <typename T, typename CharT>
class fmt::formatter<radray::AssetHandle<T>, CharT> : public fmt::formatter<radray::SparseSetHandle, CharT> {
public:
    template <typename FormatContext>
    auto format(const radray::AssetHandle<T>& handle, FormatContext& ctx) const {
        return fmt::formatter<radray::SparseSetHandle, CharT>::format({handle.GetIndex(), handle.GetGeneration()}, ctx);
    }
};
