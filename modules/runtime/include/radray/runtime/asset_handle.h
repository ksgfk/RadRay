#pragma once

#include <compare>
#include <cstdint>
#include <limits>
#include <string_view>

#include <fmt/format.h>

#include <radray/types.h>
#include <radray/uri.h>

namespace radray {
class AssetManager;
template <typename T>
class AssetRef;

class AssetId {
public:
    uint64_t Value{};
    constexpr bool IsValid() const noexcept { return Value != 0; }
    constexpr auto operator<=>(const AssetId&) const noexcept = default;
};

AssetId MakeAssetId(const Uri& uri) noexcept;

AssetId MakeAssetId(std::string_view virtualPath);

class UntypedAssetHandle {
public:
    static constexpr uint32_t kInvalidIndex = std::numeric_limits<uint32_t>::max();

    uint32_t Index{kInvalidIndex};
    uint32_t Generation{};
    constexpr bool IsValid() const noexcept { return Index != kInvalidIndex; }
    constexpr uint32_t GetIndex() const noexcept { return Index; }
    constexpr uint32_t GetGeneration() const noexcept { return Generation; }
    constexpr auto operator<=>(const UntypedAssetHandle&) const noexcept = default;
};

template <typename T>
class AssetHandle {
public:
    constexpr AssetHandle() noexcept = default;

    // 仅判断是否非空；元素是否仍存活须通过所属 AssetManager 查询。
    constexpr bool IsValid() const noexcept { return _handle.IsValid(); }
    constexpr uint32_t GetIndex() const noexcept { return _handle.GetIndex(); }
    constexpr uint32_t GetGeneration() const noexcept { return _handle.GetGeneration(); }
    constexpr UntypedAssetHandle Untyped() const noexcept { return _handle; }
    constexpr auto operator<=>(const AssetHandle&) const noexcept = default;

private:
    friend class AssetManager;
    explicit constexpr AssetHandle(UntypedAssetHandle handle) noexcept : _handle(handle) {}

    UntypedAssetHandle _handle{};
};

}  // namespace radray

template <typename T, typename CharT>
class fmt::formatter<radray::AssetHandle<T>, CharT> : public fmt::formatter<radray::string, CharT> {
public:
    template <typename FormatContext>
    auto format(const radray::AssetHandle<T>& handle, FormatContext& ctx) const {
        return fmt::format_to(ctx.out(), "AssetHandle{{Index={}, Generation={}}}", handle.GetIndex(), handle.GetGeneration());
    }
};
