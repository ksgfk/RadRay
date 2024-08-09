#pragma once

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

#include <radray/utility.h>

namespace radray::rhi {

void* RhiMalloc(size_t align, size_t size);

void RhiFree(void* ptr) noexcept;

template <class T, class... Args>
requires(!std::is_array_v<T>)
T* RhiNew(Args&&... args) {
    void* mem = RhiMalloc(alignof(T), sizeof(T));
    auto guard = MakeScopeGuard([&]() { RhiFree(mem); });
    T* obj = new (mem) T(std::forward<Args>(args)...);
    guard.Dismiss();
    return obj;
}

template <class T>
requires(!std::is_array_v<T>)
void RhiDelete(T* ptr) noexcept {
    ptr->~T();
    RhiFree(ptr);
}

template <class T>
class RhiAllocator {
public:
    using value_type = T;

    constexpr RhiAllocator() = default;
    template <class U>
    constexpr RhiAllocator(const RhiAllocator<U>&) noexcept {}

    T* allocate(std::size_t n) {
        if (n > std::numeric_limits<std::size_t>::max() / sizeof(T)) {
            throw std::bad_array_new_length();
        }
        if (auto p = static_cast<T*>(RhiMalloc(alignof(T), n * sizeof(T)))) {
            return p;
        }
        throw std::bad_alloc();
    }

    void deallocate(T* p, std::size_t n) noexcept {
        RhiFree(p);
    }
};

template <class T, class U>
bool operator==(const RhiAllocator<T>&, const RhiAllocator<U>&) { return true; }

template <class T, class U>
bool operator!=(const RhiAllocator<T>&, const RhiAllocator<U>&) { return false; }

template <class T>
requires(!std::is_array_v<T>)
class RhiDeleter {
public:
    constexpr RhiDeleter() noexcept = default;
    template <class U>
    constexpr RhiDeleter(const RhiDeleter<U>&) noexcept {}

    void operator()(T* ptr) const noexcept {
        RhiDelete(ptr);
    }
};

template <class T>
using RhiVector = std::vector<T, RhiAllocator<T>>;
template <class T>
using RhiDeque = std::deque<T, RhiAllocator<T>>;
template <class T>
using RhiQueue = std::queue<T, RhiDeque<T>>;
template <class T>
using RhiStack = std::stack<T, RhiDeque<T>>;
template <class K, class V, class Cmp = std::less<K>>
using RhiMap = std::map<K, V, Cmp, RhiAllocator<std::pair<const K, V>>>;
template <class T, class Cmp = std::less<T>>
using RhiSet = std::set<T, Cmp, RhiAllocator<T>>;
template <class K, class V, class Hash = std::hash<K>, class Equal = std::equal_to<K>>
using RhiUnorderedMap = std::unordered_map<K, V, Hash, Equal, RhiAllocator<std::pair<const K, V>>>;
template <class T, class Hash = std::hash<T>, class Equal = std::equal_to<T>>
using RhiUnorderedSet = std::unordered_set<T, Hash, Equal, RhiAllocator<T>>;
template <class T>
using RhiUniquePtr = std::unique_ptr<T, RhiDeleter<T>>;
template <class T>
using RhiSharedPtr = std::shared_ptr<T>;
template <class T>
using RhiWeakPtr = std::weak_ptr<T>;

template <class T, class... Args>
requires(!std::is_array_v<T>)
constexpr RhiUniquePtr<T> MakeUnique(Args&&... args) {
    return RhiUniquePtr<T>{RhiNew<T>(std::forward<Args>(args)...), RhiDeleter<T>{}};
}

template <class T, class... Args>
requires(!std::is_array_v<T>)
constexpr RhiSharedPtr<T> MakeShared(Args&&... args) {
    return RhiSharedPtr<T>{RhiNew<T>(std::forward<Args>(args)...), RhiDeleter<T>{}};
}

}  // namespace radray::rhi
