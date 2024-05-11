#pragma once

#ifdef RADRAY_PLATFORM_WINDOWS
#define UNICODE
#define _UNICODE
#define NOMINMAX
#define _WINDOWS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <cstddef>

namespace radray {

[[nodiscard]] void* AlignedAlloc(size_t alignment, size_t size) noexcept;

void AlignedFree(void* ptr) noexcept;

}  // namespace radray
