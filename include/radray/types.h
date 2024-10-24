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
using allocator = mi_stl_allocator<T>;
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

}  // namespace radray
