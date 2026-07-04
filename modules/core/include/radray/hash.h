#pragma once

#include <cstring>
#include <string>
#include <string_view>
#include <type_traits>

#include <radray/types.h>

namespace radray {

struct StringHash {
    using hash_type = std::hash<std::string_view>;
    using is_transparent = void;

    size_t operator()(const char* str) const noexcept { return hash_type{}(str); }
    size_t operator()(std::string_view str) const noexcept { return hash_type{}(str); }
    template <class Char, class Traits, class Alloc>
    size_t operator()(std::basic_string<Char, Traits, Alloc> const& str) const noexcept { return hash_type{}(str); }
};

struct StringEqual {
    using is_transparent = void;

    bool operator()(std::string_view lhs, std::string_view rhs) const noexcept { return lhs == rhs; }
    bool operator()(const string& lhs, std::string_view rhs) const noexcept { return std::string_view{lhs} == rhs; }
    bool operator()(std::string_view lhs, const string& rhs) const noexcept { return lhs == std::string_view{rhs}; }
    bool operator()(const string& lhs, const string& rhs) const noexcept { return lhs == rhs; }
};

size_t HashData(const void* data, size_t size) noexcept;
uint64_t HashData64(const void* data, size_t size) noexcept;

// POD 类型的通用 hash / 相等仿函数, 直接对对象内存做 byte-wise xxHash / memcmp.
// 要求 T 是 trivially copyable, 且使用者保证 padding 已清零 (例如构造时 `T key{};` 后逐字段赋值),
// 否则相同逻辑值可能因 padding 垃圾数据产生不同 hash 或不相等.
template <class T>
struct PodHasher {
    static_assert(std::is_trivially_copyable_v<T>, "PodHasher requires a trivially copyable type");
    size_t operator()(const T& value) const noexcept { return HashData(&value, sizeof(T)); }
};

template <class T>
struct PodEqual {
    static_assert(std::is_trivially_copyable_v<T>, "PodEqual requires a trivially copyable type");
    bool operator()(const T& lhs, const T& rhs) const noexcept { return std::memcmp(&lhs, &rhs, sizeof(T)) == 0; }
};

}  // namespace radray
