#pragma once

#include <cstddef>
#include <cstdint>

#include <utility>

#include <vector>
#include <deque>
#include <queue>
#include <stack>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <string>

#ifdef RADRAY_ENABLE_MIMALLOC
#include <mimalloc.h>
#endif

namespace radray {

using std::byte;
using std::int8_t;
using std::int16_t;
using std::int32_t;
using std::int64_t;
using std::uint8_t;
using std::uint16_t;
using std::uint32_t;
using std::uint64_t;

#ifdef RADRAY_ENABLE_MIMALLOC
template <class T>
class MiAllocator {
public:
    using value_type = T;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;
    using propagate_on_container_move_assignment = std::true_type;

    constexpr MiAllocator() noexcept = default;
    constexpr ~MiAllocator() noexcept = default;
    MiAllocator(const MiAllocator&) noexcept = default;
    MiAllocator(MiAllocator&&) noexcept = default;
    MiAllocator& operator=(const MiAllocator&) noexcept = default;
    MiAllocator& operator=(MiAllocator&&) noexcept = default;
    template <class U>
    MiAllocator(const MiAllocator<U>&) noexcept {}
    template <class U>
    MiAllocator& operator=(const MiAllocator<U>&) noexcept {}

    [[nodiscard]] constexpr T* allocate(size_type count) { return static_cast<T*>(mi_new_n(count, sizeof(T))); }
    constexpr void deallocate(T* p, size_type) { mi_free(p); }
};
template <class T1, class T2>
bool operator==(const MiAllocator<T1>&, const MiAllocator<T2>&) noexcept { return true; }
template <class T1, class T2>
bool operator!=(const MiAllocator<T1>&, const MiAllocator<T2>&) noexcept { return false; }

template <class T>
using allocator = MiAllocator<T>;
#else
template <class T>
using allocator = std::allocator<T>;
#endif

template <class T>
using vector = std::vector<T, radray::allocator<T>>;

template <class T>
using deque = std::deque<T, radray::allocator<T>>;

template <class T>
using queue = std::queue<T, radray::deque<T>>;

template <class T>
using stack = std::stack<T, radray::deque<T>>;

template <class K, class V, class Cmp = std::less<K>>
using map = std::map<K, V, Cmp, radray::allocator<std::pair<const K, V>>>;

template <class T, class Cmp = std::less<T>>
using set = std::set<T, Cmp, radray::allocator<T>>;

template <class K, class V, class Hash = std::hash<K>, class Equal = std::equal_to<K>>
using unordered_map = std::unordered_map<K, V, Hash, Equal, radray::allocator<std::pair<const K, V>>>;

template <class T, class Hash = std::hash<T>, class Equal = std::equal_to<T>>
using unordered_set = std::unordered_set<T, Hash, Equal, radray::allocator<T>>;

using std::unique_ptr;
using std::shared_ptr;
using std::weak_ptr;
using std::make_unique;
using std::make_shared;
using std::enable_shared_from_this;

using string = std::basic_string<char, std::char_traits<char>, radray::allocator<char>>;
using wstring = std::basic_string<wchar_t, std::char_traits<wchar_t>, radray::allocator<wchar_t>>;
using u8string = std::basic_string<char8_t, std::char_traits<char8_t>, radray::allocator<char8_t>>;
using u16string = std::basic_string<char16_t, std::char_traits<char16_t>, radray::allocator<char16_t>>;
using u32string = std::basic_string<char32_t, std::char_traits<char32_t>, radray::allocator<char32_t>>;

}  // namespace radray
