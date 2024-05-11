#include <radray/platform.h>

#ifdef RADRAY_PLATFORM_WINDOWS

#include <windows.h>

namespace radray {

void* AlignedAlloc(size_t alignment, size_t size) noexcept {
    return _aligned_malloc(size, alignment);
}

void AlignedFree(void* ptr) noexcept {
    _aligned_free(ptr);
}

}  // namespace radray

#else

#include <cstdlib>

namespace radray {

void* AlignedAlloc(size_t alignment, size_t size) noexcept {
    return std::aligned_alloc(alignment, size);
}

void AlignedFree(void* ptr) noexcept {
    return std::free(ptr);
}

}  // namespace radray

#endif
