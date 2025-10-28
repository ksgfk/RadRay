#pragma once

#include <type_traits>
#include <bitset>

#include <fmt/format.h>

#include <radray/types.h>

namespace radray {

template <class T>
struct is_flags : public std::false_type {};

template <typename T>
concept is_enum_flags = ::std::is_enum_v<T> && ::radray::is_flags<T>::value;

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

private:
    T _value;
};

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

template <class T, class CharT>
struct fmt::formatter<radray::EnumFlags<T>, CharT> : fmt::formatter<radray::string, CharT> {
    template <class FormatContext>
    auto format(radray::EnumFlags<T> const& val, FormatContext& ctx) const {
        auto v = val.value();
        std::bitset<sizeof(v) * 8> bits(v);
        radray::string binary = bits.template to_string<CharT, std::char_traits<CharT>, radray::allocator<CharT>>();
        return fmt::formatter<radray::string, CharT>::format(binary, ctx);
    }
};
