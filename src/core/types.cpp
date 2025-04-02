#include <radray/types.h>

#ifdef RADRAY_ENABLE_MIMALLOC
#include <mimalloc-new-delete.h>
#endif

namespace radray {

void* Malloc(size_t size) noexcept {
#ifdef RADRAY_ENABLE_MIMALLOC
    return mi_malloc(size);
#else
    return malloc(size);
#endif
}

void Free(void* ptr) noexcept {
#ifdef RADRAY_ENABLE_MIMALLOC
    mi_free(ptr);
#else
    free(ptr);
#endif
}

}  // namespace radray
