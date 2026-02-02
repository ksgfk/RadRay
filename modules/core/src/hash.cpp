#include <radray/hash.h>

#include <xxhash.h>

namespace radray {

namespace hash {

size_t HashData(const void* data, size_t size) noexcept {
    if constexpr (sizeof(size_t) == sizeof(uint32_t)) {
        return XXH32(data, size, 0);
    } else if constexpr (sizeof(size_t) == sizeof(uint64_t)) {
        return XXH64(data, size, 0);
    } else {
        static_assert(sizeof(size_t) == sizeof(uint32_t) || sizeof(size_t) == sizeof(uint64_t), "unknown size_t size");
    }
}

}  // namespace hash

}  // namespace radray
