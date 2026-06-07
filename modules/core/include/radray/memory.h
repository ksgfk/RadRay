#pragma once

#include <cstddef>

namespace radray {

[[nodiscard]] void* Malloc(size_t size) noexcept;

void Free(void* ptr) noexcept;

[[nodiscard]] void* AlignedAlloc(size_t alignment, size_t size) noexcept;

void AlignedFree(void* ptr) noexcept;

}  // namespace radray
