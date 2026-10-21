#include <radray/memory.h>

#include <cstdlib>

namespace radray {

void* Malloc(size_t size) noexcept {
    return std::malloc(size);
}

void Free(void* ptr) noexcept {
    std::free(ptr);
}

#ifdef RADRAY_PLATFORM_WINDOWS
void* AlignedAlloc(size_t alignment, size_t size) noexcept {
    return _aligned_malloc(size, alignment);
}

void AlignedFree(void* ptr) noexcept {
    _aligned_free(ptr);
}
#else
void* AlignedAlloc(size_t alignment, size_t size) noexcept {
    return std::aligned_alloc(alignment, size);
}

void AlignedFree(void* ptr) noexcept {
    std::free(ptr);
}
#endif

}  // namespace radray
