#pragma once

#include <type_traits>

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

}  // namespace radray::rhi
