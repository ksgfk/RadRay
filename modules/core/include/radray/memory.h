#pragma once

#include <cstddef>

namespace radray {

// Pair allocations with the corresponding RadRay free function. The selected
// allocator may differ from the caller's CRT even within the same executable.
[[nodiscard]] void* Malloc(size_t size) noexcept;

void Free(void* ptr) noexcept;

[[nodiscard]] void* AlignedAlloc(size_t alignment, size_t size) noexcept;

void AlignedFree(void* ptr) noexcept;

}  // namespace radray
