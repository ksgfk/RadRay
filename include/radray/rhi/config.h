#pragma once

#include <type_traits>

#include <radray/utility.h>

namespace radray::rhi {

template <class T, class... Args>
requires(!std::is_array_v<T>)
T* RhiNew(Args&&... args) {
    DefaultMemoryResource mr{};
    std::pmr::polymorphic_allocator<T> alloc{&mr};
    return alloc.template new_object<T>(std::forward<Args>(args)...);
    // return new T(std::forward<Args>(args)...);
}

template <class T>
requires(!std::is_array_v<T>)
void RhiDelete(T* ptr) noexcept {
    DefaultMemoryResource mr{};
    std::pmr::polymorphic_allocator<T> alloc{&mr};
    return alloc.delete_object(ptr);
    // delete ptr;
}

}  // namespace radray::rhi
