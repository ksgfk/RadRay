#pragma once

#include <type_traits>
#include <bitset>

#include <fmt/format.h>

#include <radray/types.h>

namespace radray {

namespace detail {

template <class T>
struct is_flags : public std::false_type {};

}  // namespace detail

template <class T>
requires std::is_enum_v<T>
class EnumFlags {
public:
    using enum_type = typename std::decay<T>::type;
    using underlying_type = typename std::underlying_type<T>::type;

    constexpr EnumFlags() noexcept = default;
    constexpr EnumFlags(const EnumFlags&) noexcept = default;
    constexpr EnumFlags& operator=(const EnumFlags&) noexcept = default;
    constexpr EnumFlags(EnumFlags&&) noexcept = default;
    constexpr EnumFlags& operator=(EnumFlags&&) noexcept = default;

    explicit constexpr EnumFlags(enum_type e) noexcept : _value(static_cast<underlying_type>(e)) {}
    constexpr EnumFlags& operator=(enum_type e) noexcept {
        _value = static_cast<underlying_type>(e);
        return *this;
    }

    underlying_type value() const noexcept { return _value; }

    friend constexpr bool HasFlag(EnumFlags that, enum_type l) noexcept {
        return (that._value & static_cast<underlying_type>(l)) == static_cast<underlying_type>(l);
    }

    friend constexpr bool operator==(EnumFlags l, EnumFlags r) noexcept { return l._value == r._value; }
    friend constexpr bool operator==(EnumFlags l, enum_type r) noexcept { return l._value == static_cast<underlying_type>(r); }
    friend constexpr bool operator==(enum_type l, EnumFlags r) noexcept { return static_cast<underlying_type>(l) == r._value; }

    friend constexpr bool operator!=(EnumFlags l, EnumFlags r) noexcept { return l._value != r._value; }
    friend constexpr bool operator!=(EnumFlags l, enum_type r) noexcept { return l._value != static_cast<underlying_type>(r); }
    friend constexpr bool operator!=(enum_type l, EnumFlags r) noexcept { return static_cast<underlying_type>(l) != r._value; }

    friend constexpr EnumFlags operator|(EnumFlags l, EnumFlags r) noexcept { return EnumFlags{l._value | r._value}; }
    friend constexpr EnumFlags operator|(EnumFlags l, enum_type r) noexcept { return EnumFlags{l._value | static_cast<underlying_type>(r)}; }
    friend constexpr EnumFlags operator|(enum_type l, EnumFlags r) noexcept { return EnumFlags{static_cast<underlying_type>(l) | r._value}; }

    friend constexpr EnumFlags operator&(EnumFlags l, EnumFlags r) noexcept { return EnumFlags{l._value & r._value}; }
    friend constexpr EnumFlags operator&(EnumFlags l, enum_type r) noexcept { return EnumFlags{l._value & static_cast<underlying_type>(r)}; }
    friend constexpr EnumFlags operator&(enum_type l, EnumFlags r) noexcept { return EnumFlags{static_cast<underlying_type>(l) & r._value}; }

    friend constexpr EnumFlags operator^(EnumFlags l, EnumFlags r) noexcept { return EnumFlags{l._value ^ r._value}; }
    friend constexpr EnumFlags operator^(EnumFlags l, enum_type r) noexcept { return EnumFlags{l._value ^ static_cast<underlying_type>(r)}; }
    friend constexpr EnumFlags operator^(enum_type l, EnumFlags r) noexcept { return EnumFlags{static_cast<underlying_type>(l) ^ r._value}; }

    EnumFlags& operator|=(EnumFlags v) noexcept {
        _value |= v._value;
        return *this;
    }
    EnumFlags& operator|=(enum_type v) noexcept {
        _value |= static_cast<underlying_type>(v);
        return *this;
    }

    EnumFlags& operator&=(EnumFlags v) noexcept {
        _value &= v._value;
        return *this;
    }
    EnumFlags& operator&=(enum_type v) noexcept {
        _value &= static_cast<underlying_type>(v);
        return *this;
    }

    EnumFlags& operator^=(EnumFlags v) noexcept {
        _value ^= v._value;
        return *this;
    }
    EnumFlags& operator^=(enum_type v) noexcept {
        _value ^= static_cast<underlying_type>(v);
        return *this;
    }

    constexpr EnumFlags operator~() const noexcept { return EnumFlags{~_value}; }

private:
    underlying_type _value{};
};

}  // namespace radray

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
