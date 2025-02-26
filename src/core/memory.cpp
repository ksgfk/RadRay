#include <radray/memory.h>

#include <cstdlib>
#include <cstring>

#include <radray/types.h>
#include <radray/logger.h>

namespace radray {

static void* _MemoryAllocImpl(size_t size) noexcept {
    void* ptr = nullptr;
#ifdef RADRAY_ENABLE_MIMALLOC
    ptr = mi_malloc(size);
#else
    ptr = std::malloc(size);
#endif
    if (ptr == nullptr) {
        std::abort();
    }
    return ptr;
}

static void _MemoryFreeImpl(void* ptr) noexcept {
#ifdef RADRAY_ENABLE_MIMALLOC
    mi_free(ptr);
#else
    std::free(ptr);
#endif
}

Memory::Memory() noexcept : Memory(0) {}

Memory::Memory(size_t size) noexcept : _ptr{nullptr}, _size{size} {
    if (_size > 0) {
        _ptr = _MemoryAllocImpl(_size);
    }
}

Memory::Memory(const Memory& other) noexcept : _ptr{nullptr}, _size{other._size} {
    if (_size > 0) {
        _ptr = _MemoryAllocImpl(_size);
        std::memcpy(_ptr, other._ptr, _size);
    }
}

Memory::Memory(Memory&& other) noexcept : _ptr{other._ptr}, _size{other._size} {
    other._ptr = nullptr;
    other._size = 0;
}

Memory& Memory::operator=(const Memory& other) noexcept {
    Memory tmp(other);
    swap(*this, tmp);
    return *this;
}

Memory& Memory::operator=(Memory&& other) noexcept {
    Memory tmp(std::move(other));
    swap(*this, tmp);
    return *this;
}

Memory::~Memory() noexcept {
    Destroy();
}

std::span<std::byte> Memory::GetSpan(size_t start, size_t count) const noexcept {
    RADRAY_ASSERT((start + count) <= _size);
    return {reinterpret_cast<std::byte*>(_ptr) + start, count};
}

void Memory::Allocate(size_t size) noexcept {
    if (_ptr) {
        RADRAY_ABORT("Memory already allocated");
        return;
    }
    _size = size;
    if (_size > 0) {
        _ptr = _MemoryAllocImpl(_size);
    } else {
        _ptr = nullptr;
    }
}

void Memory::Destroy() noexcept {
    if (_ptr) {
        _MemoryFreeImpl(_ptr);
        _ptr = nullptr;
    }
    _size = 0;
}

}  // namespace radray
