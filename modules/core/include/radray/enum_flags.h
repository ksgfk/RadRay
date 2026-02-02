#pragma once

#include <type_traits>
#include <limits>
#include <string>
#include <iterator>

#include <fmt/format.h>

#include <radray/types.h>
#include <radray/logger.h>

namespace radray {

template <class T>
struct is_flags : public std::false_type {};

template <typename T>
concept is_enum_flags = ::std::is_enum_v<T> && ::radray::is_flags<T>::value;

template <typename T>
concept enum_has_adl_format_as = requires(T value) {
    { format_as(value) };
};

template <class T>
requires is_enum_flags<T>
class EnumFlags {
public:
    constexpr EnumFlags() noexcept : _value{} {}

    constexpr EnumFlags(T v) noexcept : _value(v) {}

    constexpr bool HasFlag(EnumFlags f) const noexcept {
        auto v = static_cast<std::underlying_type_t<T>>(f._value);
        auto my = static_cast<std::underlying_type_t<T>>(_value);
        return (my & v) == v;
    }

    constexpr auto value() const noexcept { return static_cast<std::underlying_type_t<T>>(_value); }

    constexpr operator T() const noexcept { return _value; }

    constexpr explicit operator bool() const noexcept {
        return static_cast<std::underlying_type_t<T>>(_value) != 0;
    }

    friend constexpr bool operator==(EnumFlags l, EnumFlags r) noexcept {
        return static_cast<std::underlying_type_t<T>>(l._value) == static_cast<std::underlying_type_t<T>>(r._value);
    }

    friend constexpr bool operator==(EnumFlags l, T r) noexcept {
        return static_cast<std::underlying_type_t<T>>(l._value) == static_cast<std::underlying_type_t<T>>(r);
    }

    friend constexpr bool operator==(T l, EnumFlags r) noexcept {
        return static_cast<std::underlying_type_t<T>>(l) == static_cast<std::underlying_type_t<T>>(r._value);
    }

    friend constexpr bool operator!=(EnumFlags l, EnumFlags r) noexcept {
        return static_cast<std::underlying_type_t<T>>(l) != static_cast<std::underlying_type_t<T>>(r);
    }

    friend constexpr bool operator!=(EnumFlags l, T r) noexcept {
        return static_cast<std::underlying_type_t<T>>(l._value) != static_cast<std::underlying_type_t<T>>(r);
    }

    friend constexpr bool operator!=(T l, EnumFlags r) noexcept {
        return static_cast<std::underlying_type_t<T>>(l) != static_cast<std::underlying_type_t<T>>(r._value);
    }

    friend constexpr EnumFlags operator|(EnumFlags l, EnumFlags r) noexcept {
        return static_cast<T>(static_cast<std::underlying_type_t<T>>(l._value) | static_cast<std::underlying_type_t<T>>(r._value));
    }

    friend constexpr EnumFlags operator&(EnumFlags l, EnumFlags r) noexcept {
        return static_cast<T>(static_cast<std::underlying_type_t<T>>(l._value) & static_cast<std::underlying_type_t<T>>(r._value));
    }

    friend constexpr EnumFlags operator^(EnumFlags l, EnumFlags r) noexcept {
        return static_cast<T>(static_cast<std::underlying_type_t<T>>(l._value) ^ static_cast<std::underlying_type_t<T>>(r._value));
    }

    friend constexpr EnumFlags operator~(EnumFlags v) noexcept {
        return static_cast<T>(~static_cast<std::underlying_type_t<T>>(v._value));
    }

    constexpr EnumFlags& operator|=(EnumFlags v) noexcept {
        _value = static_cast<T>(static_cast<std::underlying_type_t<T>>(_value) | static_cast<std::underlying_type_t<T>>(v._value));
        return *this;
    }

    constexpr EnumFlags& operator&=(EnumFlags v) noexcept {
        _value = static_cast<T>(static_cast<std::underlying_type_t<T>>(_value) & static_cast<std::underlying_type_t<T>>(v._value));
        return *this;
    }

    constexpr EnumFlags& operator^=(EnumFlags v) noexcept {
        _value = static_cast<T>(static_cast<std::underlying_type_t<T>>(_value) ^ static_cast<std::underlying_type_t<T>>(v._value));
        return *this;
    }

    string FormatByName()
    requires enum_has_adl_format_as<T>
    {
        using underlying_t = std::underlying_type_t<T>;
        using unsigned_t = std::make_unsigned_t<underlying_t>;
        fmt_memory_buffer buffer;
        fmt::format_to(std::back_inserter(buffer), "[");
        auto remaining = static_cast<unsigned_t>(this->value());
        bool first = true;
        while (remaining != 0) {
            auto bit = remaining & (~remaining + 1);
            remaining &= ~bit;
            if (!first) {
                fmt::format_to(std::back_inserter(buffer), " | ");
            }
            fmt::format_to(std::back_inserter(buffer), "{}", static_cast<T>(bit));
            first = false;
        }
        fmt::format_to(std::back_inserter(buffer), "]");
        return string{buffer.data(), buffer.size()};
    }

    string FormatAsBits() {
        using underlying_t = std::underlying_type_t<T>;
        using unsigned_t = std::make_unsigned_t<underlying_t>;
        constexpr auto bit_count = std::numeric_limits<unsigned_t>::digits;
        string result;
        result.reserve(bit_count + 2);
        result.append("0b");
        auto value = static_cast<unsigned_t>(this->value());
        for (size_t index = 0; index < bit_count; ++index) {
            const bool set = (value >> (bit_count - 1 - index)) & 0x1u;
            result.push_back(set ? '1' : '0');
        }
        return result;
    }

private:
    T _value;
};

template <class T>
requires is_enum_flags<T>
string format_as(EnumFlags<T> flags) {
    using unsigned_t = std::make_unsigned_t<std::underlying_type_t<T>>;
    const auto value = static_cast<unsigned_t>(flags.value());
    if constexpr (enum_has_adl_format_as<T>) {
        if (value == 0) {
            return "[]";
        }
        return flags.FormatByName();
    } else {
        return flags.FormatAsBits();
    }
}

}  // namespace radray

template <class T>
requires radray::is_enum_flags<T>
constexpr radray::EnumFlags<T> operator|(T l, T r) noexcept {
    return static_cast<T>(static_cast<std::underlying_type_t<T>>(l) | static_cast<std::underlying_type_t<T>>(r));
}

template <class T>
requires radray::is_enum_flags<T>
constexpr radray::EnumFlags<T> operator&(T l, T r) noexcept {
    return static_cast<T>(static_cast<std::underlying_type_t<T>>(l) & static_cast<std::underlying_type_t<T>>(r));
}

template <class T>
requires radray::is_enum_flags<T>
constexpr radray::EnumFlags<T> operator^(T l, T r) noexcept {
    return static_cast<T>(static_cast<std::underlying_type_t<T>>(l) ^ static_cast<std::underlying_type_t<T>>(r));
}

template <class T>
requires radray::is_enum_flags<T>
constexpr radray::EnumFlags<T> operator~(T v) noexcept {
    return static_cast<T>(~static_cast<std::underlying_type_t<T>>(v));
}
