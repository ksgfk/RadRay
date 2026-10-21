#include <radray/hash.h>

#include <xxhash.h>

namespace radray {

size_t HashData(const void* data, size_t size) noexcept {
    if constexpr (sizeof(size_t) == sizeof(uint32_t)) {
        return XXH32(data, size, 0);
    } else if constexpr (sizeof(size_t) == sizeof(uint64_t)) {
        return XXH64(data, size, 0);
    } else {
        static_assert(sizeof(size_t) == sizeof(uint32_t) || sizeof(size_t) == sizeof(uint64_t), "unknown size_t size");
    }
}

uint64_t HashData64(const void* data, size_t size) noexcept {
    return XXH64(data, size, 0);
}

array<uint8_t, 16> HashData128(const void* data, size_t size) noexcept {
    const XXH128_hash_t value = XXH3_128bits(data, size);
    array<uint8_t, 16> result{};
    // Written little-endian by hand so the digest does not depend on the host byte order.
    for (size_t index = 0; index < 8; ++index) {
        result[index] = static_cast<uint8_t>(value.low64 >> (index * 8));
        result[index + 8] = static_cast<uint8_t>(value.high64 >> (index * 8));
    }
    return result;
}

}  // namespace radray
