#include <radray/utility.h>

#include <radray/platform.h>

namespace radray {

void* DefaultMemoryResource::do_allocate(size_t bytes, size_t align) {
    return AlignedAlloc(align, bytes);
}

void DefaultMemoryResource::do_deallocate(void* ptr, size_t bytes, size_t align) {
    AlignedFree(ptr);
}

bool DefaultMemoryResource::do_is_equal(const std::pmr::memory_resource& that) const noexcept {
    return this == &that;
}

}  // namespace radray
