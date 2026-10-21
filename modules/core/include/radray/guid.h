#pragma once

#include <array>
#include <cstddef>
#include <functional>
#include <string_view>

#include <radray/types.h>

namespace radray {

class Guid {
public:
    static constexpr size_t Size = 16;
    using ByteArray = std::array<uint8_t, Size>;

    constexpr Guid() noexcept : _bytes{} {}

    constexpr explicit Guid(const ByteArray& bytes) noexcept : _bytes(bytes) {}

    constexpr Guid(
        uint32_t data1,
        uint16_t data2,
        uint16_t data3,
        uint8_t data4_0,
        uint8_t data4_1,
        uint8_t data4_2,
        uint8_t data4_3,
        uint8_t data4_4,
        uint8_t data4_5,
        uint8_t data4_6,
        uint8_t data4_7) noexcept
        : _bytes{
              static_cast<uint8_t>((data1 >> 24) & 0xff),
              static_cast<uint8_t>((data1 >> 16) & 0xff),
              static_cast<uint8_t>((data1 >> 8) & 0xff),
              static_cast<uint8_t>(data1 & 0xff),
              static_cast<uint8_t>((data2 >> 8) & 0xff),
              static_cast<uint8_t>(data2 & 0xff),
              static_cast<uint8_t>((data3 >> 8) & 0xff),
              static_cast<uint8_t>(data3 & 0xff),
              data4_0,
              data4_1,
              data4_2,
              data4_3,
              data4_4,
              data4_5,
              data4_6,
              data4_7,
          } {}

    static constexpr Guid Empty() noexcept {
        return {};
    }
    static Guid NewGuid();

    static Guid Parse(std::string_view value);
    static bool TryParse(std::string_view value, Guid& result) noexcept;

    constexpr const ByteArray& Bytes() const noexcept {
        return _bytes;
    }

    constexpr bool IsEmpty() const noexcept {
        return *this == Guid{};
    }

    constexpr uint8_t Version() const noexcept {
        return static_cast<uint8_t>(_bytes[6] >> 4);
    }

    string ToString() const;
    string ToString(char format) const;

    constexpr bool operator==(const Guid& other) const noexcept = default;
    constexpr auto operator<=>(const Guid& other) const noexcept = default;

private:
    ByteArray _bytes;
};

string format_as(const Guid& value);

}  // namespace radray

namespace std {

template <>
struct hash<radray::Guid> {
    size_t operator()(const radray::Guid& value) const noexcept;
};

}  // namespace std
