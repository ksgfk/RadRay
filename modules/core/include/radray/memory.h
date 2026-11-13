#pragma once

#include <cstddef>
#include <memory_resource>

namespace radray {

// Pair allocations with the corresponding RadRay free function. The selected
// allocator may differ from the caller's CRT even within the same executable.
[[nodiscard]] void* Malloc(size_t size) noexcept;

void Free(void* ptr) noexcept;

[[nodiscard]] void* AlignedAlloc(size_t alignment, size_t size) noexcept;

void AlignedFree(void* ptr) noexcept;

// PMR 上游使用 RadRay 对齐堆分配；分配失败或大小溢出抛出 std::bad_alloc。
class HeapMemoryResource final : public std::pmr::memory_resource {
private:
    void* do_allocate(size_t bytes, size_t alignment) override;
    void do_deallocate(void* storage, size_t bytes, size_t alignment) override;
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override;
};

}  // namespace radray
