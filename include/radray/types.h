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
constexpr T* new_object(Args&&... args) { return new T{std::forward<Args>(args)...}; }

template <class T>
requires(std::is_array_v<T> && std::extent_v<T> == 0)
constexpr auto new_array(size_t length) { return new std::remove_extent_t<T>[length]; }

template <class T>
requires(!std::is_array_v<T>)
constexpr void delete_object(T* ptr) noexcept { delete ptr; }

template <class T>
constexpr auto delete_array(T* ptr) noexcept { delete[] ptr; }

template <class T>
class allocator {
public:
    using value_type = T;

    constexpr allocator() = default;
    template <class U>
    constexpr allocator(const allocator<U>&) noexcept {}

    constexpr T* allocate(std::size_t n) {
        auto p = static_cast<T*>(radray::mallocn(n, sizeof(T)));
        if (p == nullptr) [[unlikely]] {
            throw std::bad_alloc();
        }
        return p;
    }
    constexpr void deallocate(T* p, std::size_t) noexcept { radray::free(p); }
};
template <class T, class U>
constexpr bool operator==(const allocator<T>&, const allocator<U>&) { return true; }
template <class T, class U>
constexpr bool operator!=(const allocator<T>&, const allocator<U>&) { return false; }

template <class T>
class deleter {
public:
    constexpr deleter() noexcept = default;
    template <class U>
    constexpr deleter(const deleter<U>&) noexcept {}

    constexpr void operator()(T* ptr) const noexcept { delete_object(ptr); }
};

template <class T>
class deleter<T[]> {
public:
    constexpr deleter() noexcept = default;
    template <class U>
    constexpr deleter(const deleter<U>&) noexcept {}

    constexpr void operator()(T* ptr) const noexcept { delete_array(ptr); }
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
    return unique_ptr<T>{new_object<T>(std::forward<Args>(args)...), radray::deleter<T>{}};
}

template <class T, class... Args>
requires(std::is_array_v<T> && std::extent_v<T> == 0)
constexpr unique_ptr<T> make_unique(size_t length) {
    return unique_ptr<T>{new_array<T>(length), radray::deleter<T>{}};
}

template <class T, class... Args>
requires(!std::is_array_v<T>)
constexpr shared_ptr<T> make_shared(Args&&... args) {
    return shared_ptr<T>{new_object<T>(std::forward<Args>(args)...), radray::deleter<T>{}};
}

using string = std::basic_string<char, std::char_traits<char>, radray::allocator<char>>;

using wstring = std::basic_string<wchar_t, std::char_traits<wchar_t>, radray::allocator<wchar_t>>;

}  // namespace radray

#define RADRAY_NO_COPY_CTOR(type) \
    type(const type&) = delete;   \
    type& operator=(const type&) = delete;
#define RADRAY_NO_MOVE_CTOR(type) \
    type(type&&) = delete;        \
    type& operator=(type&&) = delete;
