#pragma once

#include <cstddef>
#include <utility>
#include <span>

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
    std::span<std::byte> GetSpan(size_t start, size_t count) const noexcept;

    void Allocate(size_t size) noexcept;
    void Destroy() noexcept;

private:
    void* _ptr;
    size_t _size;
};

}  // namespace radray
