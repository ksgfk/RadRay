#include <radray/utility.h>

#include <bit>
#include <cstring>
#include <cstdint>

namespace radray {

vector<uint32_t> ByteToDWORD(std::span<const uint8_t> bytes) noexcept {
    vector<uint32_t> result;
    if (bytes.empty()) {
        return result;
    }
    const size_t fullWords = bytes.size() / 4;
    const size_t remainder = bytes.size() % 4;
    result.resize(fullWords + (remainder ? 1 : 0));
    const auto* src = bytes.data();
    auto* dst = result.data();
    if constexpr (std::endian::native == std::endian::little) {
        if (fullWords != 0) {
            if ((reinterpret_cast<std::uintptr_t>(src) & 0x3) == 0) {
                std::memcpy(dst, src, fullWords * sizeof(uint32_t));
            } else {
                for (size_t i = 0; i < fullWords; ++i) {
                    std::memcpy(dst + i, src + i * 4, sizeof(uint32_t));
                }
            }
        }
        if (remainder != 0) {
            uint32_t last = 0;
            std::memcpy(&last, src + fullWords * 4, remainder);
            dst[fullWords] = last;
        }
    } else if constexpr (std::endian::native == std::endian::big) {
        for (size_t i = 0; i < fullWords; ++i) {
            size_t p = i * 4;
            uint32_t a = src[p];
            uint32_t b = src[p + 1];
            uint32_t c = src[p + 2];
            uint32_t d = src[p + 3];
            dst[i] = (a << 24) | (b << 16) | (c << 8) | d;
        }
        if (remainder != 0) {
            uint32_t last = 0;
            for (size_t i = 0; i < remainder; ++i) {
                last |= static_cast<uint32_t>(src[fullWords * 4 + i]) << (24 - i * 8);
            }
            dst[fullWords] = last;
        }
    } else {
        static_assert(std::endian::native == std::endian::little || std::endian::native == std::endian::big, "unknown endian platform");
    }
    return result;
}

}  // namespace radray
