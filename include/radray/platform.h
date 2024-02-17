#pragma once

namespace radray {

[[nodiscard]] void* AlignedAlloc(size_t alignment, size_t size) noexcept;

void AlignedFree(void* ptr) noexcept;

}  // namespace radray
