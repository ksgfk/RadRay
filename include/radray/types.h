#pragma once

#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>
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
#include <fmt/format.h>

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

void* RadMalloc(size_t align, size_t size);

void RadFree(void* ptr) noexcept;

template <class T, class... Args>
requires(!std::is_array_v<T>)
T* RadNew(Args&&... args) {
    void* mem = RadMalloc(alignof(T), sizeof(T));
    if (!mem) {
        throw std::bad_alloc();
    }
    auto guard = MakeScopeGuard([&]() { RadFree(mem); });
    T* obj = new (mem) T(std::forward<Args>(args)...);
    guard.Dismiss();
    return obj;
}

template <class T>
requires(!std::is_array_v<T>)
void RadDelete(T* ptr) noexcept {
    ptr->~T();
    RadFree(ptr);
}

template <class T>
class RadAllocator {
public:
    using value_type = T;

    constexpr RadAllocator() = default;
    template <class U>
    constexpr RadAllocator(const RadAllocator<U>&) noexcept {}

    T* allocate(std::size_t n) {
        if (n > std::numeric_limits<std::size_t>::max() / sizeof(T)) {
            throw std::bad_array_new_length();
        }
        if (auto p = static_cast<T*>(RadMalloc(alignof(T), n * sizeof(T)))) {
            return p;
        }
        throw std::bad_alloc();
    }

    void deallocate(T* p, std::size_t n) noexcept {
        RadFree(p);
    }
};
template <class T, class U>
bool operator==(const RadAllocator<T>&, const RadAllocator<U>&) { return true; }
template <class T, class U>
bool operator!=(const RadAllocator<T>&, const RadAllocator<U>&) { return false; }

template <class T>
requires(!std::is_array_v<T>)
class RadDeleter {
public:
    constexpr RadDeleter() noexcept = default;
    template <class U>
    constexpr RadDeleter(const RadDeleter<U>&) noexcept {}

    void operator()(T* ptr) const noexcept {
        RadDelete(ptr);
    }
};

template <class T>
using RadVector = std::vector<T, RadAllocator<T>>;

template <class T>
using RadDeque = std::deque<T, RadAllocator<T>>;

template <class T>
using RadQueue = std::queue<T, RadDeque<T>>;

template <class T>
using RadStack = std::stack<T, RadDeque<T>>;

template <class K, class V, class Cmp = std::less<K>>
using RadMap = std::map<K, V, Cmp, RadAllocator<std::pair<const K, V>>>;

template <class T, class Cmp = std::less<T>>
using RadSet = std::set<T, Cmp, RadAllocator<T>>;

template <class K, class V, class Hash = std::hash<K>, class Equal = std::equal_to<K>>
using RadUnorderedMap = std::unordered_map<K, V, Hash, Equal, RadAllocator<std::pair<const K, V>>>;

template <class T, class Hash = std::hash<T>, class Equal = std::equal_to<T>>
using RadUnorderedSet = std::unordered_set<T, Hash, Equal, RadAllocator<T>>;

template <class T>
using RadUniquePtr = std::unique_ptr<T, RadDeleter<T>>;

template <class T>
using RadSharedPtr = std::shared_ptr<T>;

template <class T>
using RadWeakPtr = std::weak_ptr<T>;

template <class T, class... Args>
requires(!std::is_array_v<T>)
constexpr RadUniquePtr<T> MakeUnique(Args&&... args) {
    return RadUniquePtr<T>{RadNew<T>(std::forward<Args>(args)...), RadDeleter<T>{}};
}

template <class T, class... Args>
requires(!std::is_array_v<T>)
constexpr RadSharedPtr<T> MakeShared(Args&&... args) {
    return RadSharedPtr<T>{RadNew<T>(std::forward<Args>(args)...), RadDeleter<T>{}};
}

using RadString = std::basic_string<char, std::char_traits<char>, RadAllocator<char>>;

using RadWString = std::basic_string<wchar_t, std::char_traits<wchar_t>, RadAllocator<wchar_t>>;

using RadFmtMemBuffer = fmt::basic_memory_buffer<char, fmt::inline_buffer_size, RadAllocator<char>>;

RadString VFormat(fmt::string_view fmtStr, fmt::format_args args) noexcept;

template <typename... Args>
RadString RFormat(fmt::string_view fmtStr, const Args&... args) {
    return VFormat(fmtStr, fmt::make_format_args(args...));
}

template <typename... Args>
RadString CFormat(fmt::format_string<Args...> fmt, const Args&... args) {
    return VFormat(fmt, fmt::make_format_args(args...));
}

}  // namespace radray

#define RADRAY_NO_COPY_CTOR(type) \
    type(const type&) = delete;   \
    type& operator=(const type&) = delete;
#define RADRAY_NO_MOVE_CTOR(type) \
    type(type&&) = delete;        \
    type& operator=(type&&) = delete;
