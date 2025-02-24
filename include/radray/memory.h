#pragma once

#include <utility>

namespace radray {

class Memory {
public:
    explicit Memory(size_t size) noexcept;
    Memory(const Memory&) noexcept;
    Memory(Memory&&) noexcept;
    Memory& operator=(const Memory&) noexcept;
    Memory& operator=(Memory&&) noexcept;
    ~Memory() noexcept;

    friend constexpr void swap(Memory& lhs, Memory& rhs) noexcept {
        std::swap(lhs._ptr, rhs._ptr);
        std::swap(lhs._size, rhs._size);
    }

private:
    void* _ptr;
    size_t _size;
};

}  // namespace radray
