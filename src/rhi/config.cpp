#include <radray/rhi/config.h>

#include <radray/platform.h>

namespace radray::rhi {

void* RhiMalloc(size_t align, size_t size) {
    return AlignedAlloc(align, size);
}

void RhiFree(void* ptr) noexcept {
    AlignedFree(ptr);
}

}  // namespace radray::rhi
