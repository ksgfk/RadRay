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

[[nodiscard]] void* malloc(size_t size) noexcept;
[[nodiscard]] void* mallocn(size_t count, size_t size) noexcept;
void free(void* ptr) noexcept;
void free_size(void* ptr, size_t size) noexcept;
[[nodiscard]] void* aligned_alloc(size_t alignment, size_t size) noexcept;
void aligned_free(void* ptr, size_t alignment) noexcept;
void aligned_free_size(void* ptr, size_t size, size_t alignment) noexcept;

template <class T, class... Args>
requires(!std::is_array_v<T>)
T* new_object(Args&&... args) {
    void* mem = radray::aligned_alloc(alignof(T), sizeof(T));
    if (!mem) [[unlikely]] {
        throw std::bad_alloc();
    }
    auto guard = MakeScopeGuard([&]() { radray::free(mem); });
    T* obj = std::construct_at(mem, std::forward<Args>(args)...);
    guard.Dismiss();
    return obj;
}

template <class T>
requires(!std::is_array_v<T>)
void delete_object(T* ptr) noexcept {
    std::destroy_at(ptr);
    radray::free(ptr);
}

template <class T>
class allocator {
public:
    using value_type = T;

    constexpr allocator() = default;
    template <class U>
    constexpr allocator(const allocator<U>&) noexcept {}

    T* allocate(std::size_t n) {
        auto p = static_cast<T*>(radray::mallocn(n, sizeof(T)));
        if (p == nullptr) [[unlikely]] {
            throw std::bad_alloc();
        }
        return p;
    }

    void deallocate(T* p, std::size_t) noexcept { radray::free(p); }
};
template <class T, class U>
bool operator==(const allocator<T>&, const allocator<U>&) { return true; }
template <class T, class U>
bool operator!=(const allocator<T>&, const allocator<U>&) { return false; }

template <class T>
requires(!std::is_array_v<T>)
class deleter {
public:
    constexpr deleter() noexcept = default;
    template <class U>
    constexpr deleter(const deleter<U>&) noexcept {}

    void operator()(T* ptr) const noexcept { delete_object(ptr); }
};

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

template <class T>
using unique_ptr = std::unique_ptr<T, radray::deleter<T>>;

template <class T>
using shared_ptr = std::shared_ptr<T>;

template <class T>
using weak_ptr = std::weak_ptr<T>;

template <class T, class... Args>
requires(!std::is_array_v<T>)
constexpr unique_ptr<T> make_unique(Args&&... args) {
    return unique_ptr<T>{new_object<T>(std::forward<Args>(args)...), deleter<T>{}};
}

template <class T, class... Args>
requires(!std::is_array_v<T>)
constexpr shared_ptr<T> make_shared(Args&&... args) {
    return shared_ptr<T>{new_object<T>(std::forward<Args>(args)...), deleter<T>{}};
}

using string = std::basic_string<char, std::char_traits<char>, radray::allocator<char>>;

using wstring = std::basic_string<wchar_t, std::char_traits<wchar_t>, radray::allocator<wchar_t>>;

using fmt_memory_buffer = fmt::basic_memory_buffer<char, fmt::inline_buffer_size, radray::allocator<char>>;

string v_format(fmt::string_view fmtStr, fmt::format_args args) noexcept;

template <typename... Args>
string format(fmt::string_view fmtStr, Args&&... args) {
    return radray::v_format(fmtStr, fmt::make_format_args(std::forward<Args>(args)...));
}

template <typename... Args>
string format_to(fmt::format_string<Args...> fmt, Args&&... args) {
    return radray::v_format(fmt::string_view(fmt), fmt::make_format_args(std::forward<Args>(args)...));
}

}  // namespace radray

#define RADRAY_NO_COPY_CTOR(type) \
    type(const type&) = delete;   \
    type& operator=(const type&) = delete;
#define RADRAY_NO_MOVE_CTOR(type) \
    type(type&&) = delete;        \
    type& operator=(type&&) = delete;

void operator delete(void* p) noexcept;
void operator delete[](void* p) noexcept;

void operator delete(void* p, const std::nothrow_t&) noexcept;
void operator delete[](void* p, const std::nothrow_t&) noexcept;

void* operator new(std::size_t n) noexcept(false);
void* operator new[](std::size_t n) noexcept(false);

void* operator new(std::size_t n, const std::nothrow_t& tag) noexcept;
void* operator new[](std::size_t n, const std::nothrow_t& tag) noexcept;

void operator delete(void* p, std::size_t n) noexcept;
void operator delete[](void* p, std::size_t n) noexcept;

void operator delete(void* p, std::align_val_t al) noexcept;
void operator delete[](void* p, std::align_val_t al) noexcept;
void operator delete(void* p, std::size_t n, std::align_val_t al) noexcept;
void operator delete[](void* p, std::size_t n, std::align_val_t al) noexcept;
void operator delete(void* p, std::align_val_t al, const std::nothrow_t&) noexcept;
void operator delete[](void* p, std::align_val_t al, const std::nothrow_t&) noexcept;

void* operator new(std::size_t n, std::align_val_t al) noexcept(false);
void* operator new[](std::size_t n, std::align_val_t al) noexcept(false);
void* operator new(std::size_t n, std::align_val_t al, const std::nothrow_t&) noexcept;
void* operator new[](std::size_t n, std::align_val_t al, const std::nothrow_t&) noexcept;
