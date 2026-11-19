#include <radray/hash.h>

#include <iterator>

#include <fmt/format.h>
#include <xxhash.h>

namespace radray {

Hash128 Hash128::FromWords(uint64_t low, uint64_t high) noexcept {
    Hash128 digest{};
    for (size_t index = 0; index < 8; ++index) {
        digest.Bytes[index] = static_cast<uint8_t>(low >> (index * 8));
        digest.Bytes[index + 8] = static_cast<uint8_t>(high >> (index * 8));
    }
    return digest;
}

string Hash128::ToHex() const {
    string result;
    result.reserve(Bytes.size() * 2);
    for (const uint8_t value : Bytes) {
        fmt::format_to(std::back_inserter(result), "{:02x}", value);
    }
    return result;
}

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

Hash128 HashData128(const void* data, size_t size) noexcept {
    const XXH128_hash_t value = XXH3_128bits(data, size);
    return Hash128::FromWords(value.low64, value.high64);
}

size_t StringHash::operator()(const char* str) const noexcept {
    return hash_type{}(str);
}

size_t StringHash::operator()(std::string_view str) const noexcept {
    return hash_type{}(str);
}

bool StringEqual::operator()(std::string_view lhs, std::string_view rhs) const noexcept {
    return lhs == rhs;
}

bool StringEqual::operator()(const string& lhs, std::string_view rhs) const noexcept {
    return std::string_view{lhs} == rhs;
}

bool StringEqual::operator()(std::string_view lhs, const string& rhs) const noexcept {
    return lhs == std::string_view{rhs};
}

bool StringEqual::operator()(const string& lhs, const string& rhs) const noexcept {
    return lhs == rhs;
}

}  // namespace radray
