#pragma once

#include <cstddef>
#include <utility>
#include <span>

#include <radray/logger.h>

namespace radray {

class Memory {
public:
    Memory() noexcept;
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

    void* GetData() const noexcept { return _ptr; }
    size_t GetSize() const noexcept { return _size; }
    std::span<std::byte> GetSpan(size_t start, size_t count) const noexcept {
        return GetSpan<std::byte>(start, count);
    }
    template <class T>
    std::span<T> GetSpan(size_t start, size_t count) const noexcept {
        constexpr size_t size = sizeof(T);
        const size_t x = start * size;
        const size_t y = count * size;
        RADRAY_ASSERT((x + y) <= _size);
        return std::span<T>{reinterpret_cast<T*>(_ptr) + x, y};
    }

    void Allocate(size_t size) noexcept;
    void Destroy() noexcept;

private:
    void* _ptr;
    size_t _size;
};

}  // namespace radray
