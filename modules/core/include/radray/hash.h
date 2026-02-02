#pragma once

#include <string>
#include <string_view>
#include <utility>

namespace radray {

struct StringHash {
    using hash_type = std::hash<std::string_view>;
    using is_transparent = void;

    size_t operator()(const char* str) const noexcept { return hash_type{}(str); }
    size_t operator()(std::string_view str) const noexcept { return hash_type{}(str); }
    template <class Char, class Traits, class Alloc>
    size_t operator()(std::basic_string<Char, Traits, Alloc> const& str) const noexcept { return hash_type{}(str); }
};

namespace hash {

size_t HashData(const void* data, size_t size) noexcept;

}

}  // namespace radray
