#pragma once

#include <bit>
#include <cstring>
#include <functional>
#include <string>
#include <string_view>
#include <type_traits>

#include <radray/types.h>

namespace radray {

namespace detail {

// .NET System.HashCode.Combine 使用的 xxHash32 常量。
inline constexpr uint32_t kPrime1_32 = 2654435761u;
inline constexpr uint32_t kPrime2_32 = 2246822519u;
inline constexpr uint32_t kPrime3_32 = 3266489917u;
inline constexpr uint32_t kPrime4_32 = 668265263u;
inline constexpr uint32_t kPrime5_32 = 374761393u;

inline constexpr uint64_t kPrime1_64 = 11400714785074694791ull;
inline constexpr uint64_t kPrime2_64 = 14029467366897019727ull;
inline constexpr uint64_t kPrime3_64 = 1609587929392839161ull;
inline constexpr uint64_t kPrime4_64 = 9650029242287828579ull;
inline constexpr uint64_t kPrime5_64 = 2870177450012600261ull;

constexpr uint32_t QueueRound32(uint32_t hash, uint32_t value) noexcept {
    return std::rotl(hash + value * kPrime3_32, 17) * kPrime4_32;
}

constexpr uint32_t MixFinal32(uint32_t hash) noexcept {
    hash ^= hash >> 15;
    hash *= kPrime2_32;
    hash ^= hash >> 13;
    hash *= kPrime3_32;
    hash ^= hash >> 16;
    return hash;
}

constexpr uint32_t MixState32(uint32_t v1, uint32_t v2, uint32_t v3, uint32_t v4) noexcept {
    return std::rotl(v1, 1) + std::rotl(v2, 7) + std::rotl(v3, 12) + std::rotl(v4, 18);
}

constexpr uint32_t Combine32(uint32_t lhs, uint32_t rhs) noexcept {
    uint32_t hash = kPrime5_32 + 8u;
    hash = QueueRound32(hash, lhs);
    hash = QueueRound32(hash, rhs);
    return MixFinal32(hash);
}

constexpr uint64_t Round64(uint64_t hash, uint64_t value) noexcept {
    return std::rotl(hash + value * kPrime2_64, 31) * kPrime1_64;
}

constexpr uint64_t MergeRound64(uint64_t hash, uint64_t value) noexcept {
    hash ^= Round64(0, value);
    return hash * kPrime1_64 + kPrime4_64;
}

constexpr uint64_t FinalizeRound64(uint64_t hash, uint64_t value) noexcept {
    hash ^= Round64(0, value);
    return std::rotl(hash, 27) * kPrime1_64 + kPrime4_64;
}

constexpr uint64_t MixState64(uint64_t v1, uint64_t v2, uint64_t v3, uint64_t v4) noexcept {
    uint64_t hash = std::rotl(v1, 1) + std::rotl(v2, 7) + std::rotl(v3, 12) + std::rotl(v4, 18);
    hash = MergeRound64(hash, v1);
    hash = MergeRound64(hash, v2);
    hash = MergeRound64(hash, v3);
    hash = MergeRound64(hash, v4);
    return hash;
}

constexpr uint64_t MixFinal64(uint64_t hash) noexcept {
    hash ^= hash >> 33;
    hash *= kPrime2_64;
    hash ^= hash >> 29;
    hash *= kPrime3_64;
    hash ^= hash >> 32;
    return hash;
}

constexpr uint64_t Combine64(uint64_t lhs, uint64_t rhs) noexcept {
    uint64_t hash = kPrime5_64 + 16u;
    hash = FinalizeRound64(hash, lhs);
    hash = FinalizeRound64(hash, rhs);
    return MixFinal64(hash);
}

}  // namespace detail

class HashCode {
public:
    constexpr HashCode() noexcept = default;
    constexpr explicit HashCode(size_t seed) noexcept : _seed(seed) {}

    constexpr void Add(size_t value) noexcept {
        if constexpr (sizeof(size_t) == sizeof(uint32_t)) {
            Add32(static_cast<uint32_t>(value));
        } else {
            Add64(static_cast<uint64_t>(value));
        }
    }

    template <typename T>
    requires std::is_integral_v<T> && (!std::is_same_v<T, size_t>)
    constexpr void Add(T value) noexcept {
        Add(static_cast<size_t>(value));
    }

    template <typename T>
    requires(!std::is_integral_v<T>) && requires(const T& value) { std::hash<T>{}(value); }
    void Add(const T& value) noexcept(noexcept(std::hash<T>{}(value))) {
        Add(static_cast<size_t>(std::hash<T>{}(value)));
    }

    constexpr size_t ToHashCode() const noexcept {
        if constexpr (sizeof(size_t) == sizeof(uint32_t)) {
            return static_cast<size_t>(Finalize32());
        } else {
            return static_cast<size_t>(Finalize64());
        }
    }

    template <typename T>
    requires std::is_integral_v<T> && std::is_unsigned_v<T> && (sizeof(T) == 4 || sizeof(T) == 8)
    static constexpr T Combine(T lhs, T rhs) noexcept {
        if constexpr (sizeof(T) == sizeof(uint32_t)) {
            return static_cast<T>(detail::Combine32(static_cast<uint32_t>(lhs), static_cast<uint32_t>(rhs)));
        } else {
            return static_cast<T>(detail::Combine64(static_cast<uint64_t>(lhs), static_cast<uint64_t>(rhs)));
        }
    }

private:
    constexpr uint32_t Seed32() const noexcept { return static_cast<uint32_t>(_seed); }
    constexpr uint64_t Seed64() const noexcept { return static_cast<uint64_t>(_seed); }

    constexpr void Initialize32() noexcept {
        _v1 = Seed32() + detail::kPrime1_32 + detail::kPrime2_32;
        _v2 = Seed32() + detail::kPrime2_32;
        _v3 = Seed32();
        _v4 = Seed32() - detail::kPrime1_32;
    }

    constexpr void Add32(uint32_t value) noexcept {
        const uint32_t previousLength = static_cast<uint32_t>(_length++);
        switch (previousLength & 3u) {
            case 0:
                _queue1 = value;
                break;
            case 1:
                _queue2 = value;
                break;
            case 2:
                _queue3 = value;
                break;
            default:
                if (previousLength == 3u) {
                    Initialize32();
                }
                _v1 = std::rotl(
                          static_cast<uint32_t>(_v1) +
                              static_cast<uint32_t>(_queue1) * detail::kPrime2_32,
                          13) *
                      detail::kPrime1_32;
                _v2 = std::rotl(
                          static_cast<uint32_t>(_v2) +
                              static_cast<uint32_t>(_queue2) * detail::kPrime2_32,
                          13) *
                      detail::kPrime1_32;
                _v3 = std::rotl(
                          static_cast<uint32_t>(_v3) +
                              static_cast<uint32_t>(_queue3) * detail::kPrime2_32,
                          13) *
                      detail::kPrime1_32;
                _v4 = std::rotl(static_cast<uint32_t>(_v4) + value * detail::kPrime2_32, 13) *
                      detail::kPrime1_32;
                break;
        }
    }

    constexpr uint32_t Finalize32() const noexcept {
        const uint32_t length = static_cast<uint32_t>(_length);
        const uint32_t position = length & 3u;
        uint32_t hash = length < 4u
                            ? Seed32() + detail::kPrime5_32
                            : detail::MixState32(
                                  static_cast<uint32_t>(_v1),
                                  static_cast<uint32_t>(_v2),
                                  static_cast<uint32_t>(_v3),
                                  static_cast<uint32_t>(_v4));
        hash += length * 4u;
        if (position > 0u) {
            hash = detail::QueueRound32(hash, static_cast<uint32_t>(_queue1));
            if (position > 1u) {
                hash = detail::QueueRound32(hash, static_cast<uint32_t>(_queue2));
                if (position > 2u) {
                    hash = detail::QueueRound32(hash, static_cast<uint32_t>(_queue3));
                }
            }
        }
        return detail::MixFinal32(hash);
    }

    constexpr void Initialize64() noexcept {
        _v1 = Seed64() + detail::kPrime1_64 + detail::kPrime2_64;
        _v2 = Seed64() + detail::kPrime2_64;
        _v3 = Seed64();
        _v4 = Seed64() - detail::kPrime1_64;
    }

    constexpr void Add64(uint64_t value) noexcept {
        const uint64_t previousLength = _length++;
        switch (previousLength & 3u) {
            case 0:
                _queue1 = value;
                break;
            case 1:
                _queue2 = value;
                break;
            case 2:
                _queue3 = value;
                break;
            default:
                if (previousLength == 3u) {
                    Initialize64();
                }
                _v1 = detail::Round64(_v1, _queue1);
                _v2 = detail::Round64(_v2, _queue2);
                _v3 = detail::Round64(_v3, _queue3);
                _v4 = detail::Round64(_v4, value);
                break;
        }
    }

    constexpr uint64_t Finalize64() const noexcept {
        const uint64_t length = _length;
        const uint64_t position = length & 3u;
        uint64_t hash = length < 4u
                            ? Seed64() + detail::kPrime5_64
                            : detail::MixState64(_v1, _v2, _v3, _v4);
        hash += length * 8u;
        if (position > 0u) {
            hash = detail::FinalizeRound64(hash, _queue1);
            if (position > 1u) {
                hash = detail::FinalizeRound64(hash, _queue2);
                if (position > 2u) {
                    hash = detail::FinalizeRound64(hash, _queue3);
                }
            }
        }
        return detail::MixFinal64(hash);
    }

    size_t _seed{0};
    size_t _length{0};
    size_t _v1{0}, _v2{0}, _v3{0}, _v4{0};
    size_t _queue1{0}, _queue2{0}, _queue3{0};
};

struct StringHash {
    using hash_type = std::hash<std::string_view>;
    using is_transparent = void;

    size_t operator()(const char* str) const noexcept { return hash_type{}(str); }
    size_t operator()(std::string_view str) const noexcept { return hash_type{}(str); }
    template <class Char, class Traits, class Alloc>
    size_t operator()(std::basic_string<Char, Traits, Alloc> const& str) const noexcept { return hash_type{}(str); }
};

struct StringEqual {
    using is_transparent = void;

    bool operator()(std::string_view lhs, std::string_view rhs) const noexcept { return lhs == rhs; }
    bool operator()(const string& lhs, std::string_view rhs) const noexcept { return std::string_view{lhs} == rhs; }
    bool operator()(std::string_view lhs, const string& rhs) const noexcept { return lhs == std::string_view{rhs}; }
    bool operator()(const string& lhs, const string& rhs) const noexcept { return lhs == rhs; }
};

size_t HashData(const void* data, size_t size) noexcept;
uint64_t HashData64(const void* data, size_t size) noexcept;

template <class T>
struct PodHasher {
    static_assert(std::is_trivially_copyable_v<T>, "PodHasher requires a trivially copyable type");
    size_t operator()(const T& value) const noexcept { return HashData(&value, sizeof(T)); }
};

template <class T>
struct PodEqual {
    static_assert(std::is_trivially_copyable_v<T>, "PodEqual requires a trivially copyable type");
    bool operator()(const T& lhs, const T& rhs) const noexcept { return std::memcmp(&lhs, &rhs, sizeof(T)) == 0; }
};

}  // namespace radray
