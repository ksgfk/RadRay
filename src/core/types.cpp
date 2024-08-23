#include <radray/types.h>

#ifdef RADRAY_ENABLE_MIMALLOC
#include <mimalloc.h>
#endif

#include <cstdlib>
#include <radray/platform.h>

namespace radray {

#ifdef RADRAY_ENABLE_MIMALLOC
void* malloc(size_t size) noexcept { return mi_malloc(size); }
void* mallocn(size_t count, size_t size) noexcept { return mi_mallocn(count, size); }
void free(void* ptr) noexcept { mi_free(ptr); }
void free_size(void* ptr, size_t size) noexcept { mi_free_size(ptr, size); }
void* aligned_alloc(size_t alignment, size_t size) noexcept { return mi_malloc_aligned(size, alignment); }
void aligned_free(void* ptr, size_t alignment) noexcept { mi_free_aligned(ptr, alignment); }
void aligned_free_size(void* ptr, size_t size, size_t alignment) noexcept { mi_free_size_aligned(ptr, size, alignment); }
#else
void* malloc(size_t size) noexcept { return std::malloc(size); }
void* mallocn(size_t count, size_t size) noexcept {
    if (count > std::numeric_limits<std::size_t>::max() / size) {
        return nullptr;
    }
    return std::malloc(count * size);
}
void free(void* ptr) noexcept { std::free(ptr); }
void free_size(void* ptr, size_t size) noexcept { std::free(ptr); }
void* aligned_alloc(size_t alignment, size_t size) noexcept { return AlignedAlloc(alignment, size); }
void aligned_free(void* ptr, size_t alignment) noexcept { AlignedFree(ptr); }
void aligned_free_size(void* ptr, size_t size, size_t alignment) noexcept { AlignedFree(ptr); }
#endif

string v_format(fmt::string_view fmtStr, fmt::format_args args) noexcept {
    fmt_memory_buffer buf{};
    fmt::vformat_to(std::back_inserter(buf), fmtStr, args);
    return string{buf.data(), buf.size()};
}

}  // namespace radray

void operator delete(void* p) noexcept { radray::free(p); };
void operator delete[](void* p) noexcept { radray::free(p); };

void operator delete(void* p, const std::nothrow_t&) noexcept { radray::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { radray::free(p); }

void* operator new(std::size_t n) noexcept(false) {
    void* ptr = radray::malloc(n);
    if (ptr == nullptr) [[unlikely]] {
        throw std::bad_alloc();
    }
    return ptr;
}
void* operator new[](std::size_t n) noexcept(false) {
    void* ptr = radray::malloc(n);
    if (ptr == nullptr) [[unlikely]] {
        throw std::bad_alloc();
    }
    return ptr;
}

void* operator new(std::size_t n, const std::nothrow_t& tag) noexcept {
    (void)(tag);
    return radray::malloc(n);
}
void* operator new[](std::size_t n, const std::nothrow_t& tag) noexcept {
    (void)(tag);
    return radray::malloc(n);
}

void operator delete(void* p, std::size_t n) noexcept { radray::free_size(p, n); };
void operator delete[](void* p, std::size_t n) noexcept { radray::free_size(p, n); };

void operator delete(void* p, std::align_val_t al) noexcept { radray::aligned_free(p, static_cast<size_t>(al)); }
void operator delete[](void* p, std::align_val_t al) noexcept { radray::aligned_free(p, static_cast<size_t>(al)); }
void operator delete(void* p, std::size_t n, std::align_val_t al) noexcept { radray::aligned_free_size(p, n, static_cast<size_t>(al)); };
void operator delete[](void* p, std::size_t n, std::align_val_t al) noexcept { radray::aligned_free_size(p, n, static_cast<size_t>(al)); };
void operator delete(void* p, std::align_val_t al, const std::nothrow_t&) noexcept { radray::aligned_free(p, static_cast<size_t>(al)); }
void operator delete[](void* p, std::align_val_t al, const std::nothrow_t&) noexcept { radray::aligned_free(p, static_cast<size_t>(al)); }

void* operator new(std::size_t n, std::align_val_t al) noexcept(false) {
    void* ptr = radray::aligned_alloc(static_cast<size_t>(al), n);
    if (ptr == nullptr) [[unlikely]] {
        throw std::bad_alloc();
    }
    return ptr;
}
void* operator new[](std::size_t n, std::align_val_t al) noexcept(false) {
    void* ptr = radray::aligned_alloc(static_cast<size_t>(al), n);
    if (ptr == nullptr) [[unlikely]] {
        throw std::bad_alloc();
    }
    return ptr;
}
void* operator new(std::size_t n, std::align_val_t al, const std::nothrow_t&) noexcept { return radray::aligned_alloc(n, static_cast<size_t>(al)); }
void* operator new[](std::size_t n, std::align_val_t al, const std::nothrow_t&) noexcept { return radray::aligned_alloc(n, static_cast<size_t>(al)); }
