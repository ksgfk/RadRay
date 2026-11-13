#include <radray/memory.h>

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <new>

#include <radray/nullable.h>

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

void* HeapMemoryResource::do_allocate(size_t bytes, size_t alignment) {
    alignment = std::max(alignment, alignof(std::max_align_t));
    bytes = std::max(bytes, size_t{1});
    Nullable<void*> storage{nullptr};
    if (bytes <= std::numeric_limits<size_t>::max() - (alignment - 1)) {
        const size_t alignedBytes = (bytes + alignment - 1) & ~(alignment - 1);
        storage = AlignedAlloc(alignment, alignedBytes);
    }
    if (!storage) throw std::bad_alloc{};
    return storage.Get();
}

void HeapMemoryResource::do_deallocate(void* storage, size_t, size_t) {
    AlignedFree(storage);
}

bool HeapMemoryResource::do_is_equal(const std::pmr::memory_resource& other) const noexcept {
    return this == &other;
}

}  // namespace radray
