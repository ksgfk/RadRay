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
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-copy"
#include <mimalloc.h>
#pragma clang diagnostic pop
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
using allocator = mi_stl_allocator<T>;
#else
template <class T>
using allocator = std::allocator<T>;
#endif

template <class T>
using vector = std::vector<T, allocator<T>>;

template <class T>
using deque = std::deque<T, allocator<T>>;

template <class T>
using queue = std::queue<T, deque<T>>;

template <class T>
using stack = std::stack<T, deque<T>>;

template <class K, class V, class Cmp = std::less<K>>
using map = std::map<K, V, Cmp, allocator<std::pair<const K, V>>>;

template <class T, class Cmp = std::less<T>>
using set = std::set<T, Cmp, allocator<T>>;

template <class K, class V, class Hash = std::hash<K>, class Equal = std::equal_to<K>>
using unordered_map = std::unordered_map<K, V, Hash, Equal, allocator<std::pair<const K, V>>>;

template <class T, class Hash = std::hash<T>, class Equal = std::equal_to<T>>
using unordered_set = std::unordered_set<T, Hash, Equal, allocator<T>>;

template <class K, class V, class Cmp = std::less<K>>
using multimap = std::multimap<K, V, Cmp, allocator<std::pair<const K, V>>>;

template <class T, class Cmp = std::less<T>>
using multiset = std::multiset<T, Cmp, allocator<T>>;

using std::unique_ptr;
using std::shared_ptr;
using std::weak_ptr;
using std::make_unique;
using std::make_shared;
using std::enable_shared_from_this;

using string = std::basic_string<char, std::char_traits<char>, allocator<char>>;
using wstring = std::basic_string<wchar_t, std::char_traits<wchar_t>, allocator<wchar_t>>;
using u8string = std::basic_string<char8_t, std::char_traits<char8_t>, allocator<char8_t>>;
using u16string = std::basic_string<char16_t, std::char_traits<char16_t>, allocator<char16_t>>;
using u32string = std::basic_string<char32_t, std::char_traits<char32_t>, allocator<char32_t>>;

}  // namespace radray
