#include <radray/memory.h>

#include <cstdlib>

#ifdef RADRAY_ENABLE_MIMALLOC
#include <mimalloc.h>
#endif

namespace radray {

#ifdef RADRAY_ENABLE_MIMALLOC
void* Malloc(size_t size) noexcept {
    return mi_malloc(size);
}

void Free(void* ptr) noexcept {
    mi_free(ptr);
}

void* AlignedAlloc(size_t alignment, size_t size) noexcept {
    return mi_malloc_aligned(size, alignment);
}

void AlignedFree(void* ptr) noexcept {
    mi_free(ptr);
}
#else
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
#endif

}  // namespace radray
