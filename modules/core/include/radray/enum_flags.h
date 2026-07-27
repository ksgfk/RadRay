#pragma once

#include <type_traits>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

#include <fmt/format.h>
#include <magic_enum/magic_enum_flags.hpp>

#include <radray/types.h>

namespace radray {

template <class T>
struct is_flags : public std::false_type {};

template <class T>
struct is_compound_enum_flags : public std::false_type {};

template <typename T>
concept is_enum_flags = ::std::is_enum_v<T> && ::radray::is_flags<T>::value;

template <typename T>
concept enum_has_adl_format_as = requires(T value) {
    { format_as(value) };
};

// ---------------------------------------------------------------------------
// 枚举名字工具。这里是 magic_enum 的唯一出口, 其他地方只 include 本头文件,
// 不要直接 include magic_enum, 以便底层库可替换。
// ---------------------------------------------------------------------------

// 枚举成员名。取不到名字 (值越界或无对应成员) 时返回空。
// 反射范围默认是 [-128, 127], 超出该范围的位标志枚举请用 EnumFlagBitName。
template <class T>
requires std::is_enum_v<T>
constexpr std::string_view EnumName(T value) noexcept {
    return magic_enum::enum_name(value);
}

// 同 EnumName, 取不到名字时返回 fallback。
template <class T>
requires std::is_enum_v<T>
constexpr std::string_view EnumNameOr(T value, std::string_view fallback = "UNKNOWN") noexcept {
    const std::string_view name = magic_enum::enum_name(value);
    return name.empty() ? fallback : name;
}

// 单个位标志的成员名。按 1, 2, 4, ... 逐位反射, 因此不受 [-128, 127] 限制,
// 但只能匹配单 bit 成员: 0 与复合值 (多 bit) 都取不到名字。
template <class T>
requires std::is_enum_v<T>
constexpr std::string_view EnumFlagBitName(T value) noexcept {
    return magic_enum::enum_name<T, magic_enum::as_flags<>>(value);
}

// 同 EnumFlagBitName, 取不到名字时返回 fallback。
template <class T>
requires std::is_enum_v<T>
constexpr std::string_view EnumFlagBitNameOr(T value, std::string_view fallback = "UNKNOWN") noexcept {
    const std::string_view name = EnumFlagBitName(value);
    return name.empty() ? fallback : name;
}

// 把置位的各个标志名用 sep 连接。存在无名的位时返回空。
template <class T>
requires std::is_enum_v<T>
string EnumFlagsName(T value, char sep = '|') {
    return magic_enum::enum_flags_name(value, sep);
}

// 该值是否是枚举里已声明的成员。
template <class T>
requires std::is_enum_v<T>
constexpr bool EnumContains(T value) noexcept {
    return magic_enum::enum_contains(value);
}

// 由名字反查枚举值。
template <class T>
requires std::is_enum_v<T>
constexpr std::optional<T> EnumCast(std::string_view name) noexcept {
    return magic_enum::enum_cast<T>(name);
}

// 由名字 (可含 sep 分隔的多个标志) 反查位标志枚举值。
template <class T>
requires std::is_enum_v<T>
constexpr std::optional<T> EnumFlagsCast(std::string_view name, char sep = '|') noexcept {
    return magic_enum::enum_flags_cast<T>(name, sep);
}

// 由整数反查位标志枚举值。含未定义的位时返回 nullopt。
template <class T>
requires std::is_enum_v<T>
constexpr std::optional<T> EnumFlagsCast(std::underlying_type_t<T> value) noexcept {
    return magic_enum::enum_flags_cast<T>(value);
}

template <class T>
requires is_enum_flags<T>
class EnumFlags {
public:
    constexpr EnumFlags() noexcept : _value{} {}

    constexpr EnumFlags(T v) noexcept : _value(v) {}

    constexpr explicit EnumFlags(std::underlying_type_t<T> v) noexcept : _value(static_cast<T>(v)) {}

    template <class U>
    requires std::is_convertible_v<U, T>
    constexpr explicit EnumFlags(U v) noexcept : _value(static_cast<T>(v)) {}

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
        return static_cast<std::underlying_type_t<T>>(l._value) != static_cast<std::underlying_type_t<T>>(r._value);
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

    friend constexpr EnumFlags operator|(EnumFlags l, T r) noexcept {
        return static_cast<T>(static_cast<std::underlying_type_t<T>>(l._value) | static_cast<std::underlying_type_t<T>>(r));
    }

    friend constexpr EnumFlags operator|(T l, EnumFlags r) noexcept {
        return static_cast<T>(static_cast<std::underlying_type_t<T>>(l) | static_cast<std::underlying_type_t<T>>(r._value));
    }

    friend constexpr EnumFlags operator&(EnumFlags l, EnumFlags r) noexcept {
        return static_cast<T>(static_cast<std::underlying_type_t<T>>(l._value) & static_cast<std::underlying_type_t<T>>(r._value));
    }

    friend constexpr EnumFlags operator&(EnumFlags l, T r) noexcept {
        return static_cast<T>(static_cast<std::underlying_type_t<T>>(l._value) & static_cast<std::underlying_type_t<T>>(r));
    }

    friend constexpr EnumFlags operator&(T l, EnumFlags r) noexcept {
        return static_cast<T>(static_cast<std::underlying_type_t<T>>(l) & static_cast<std::underlying_type_t<T>>(r._value));
    }

    friend constexpr EnumFlags operator^(EnumFlags l, EnumFlags r) noexcept {
        return static_cast<T>(static_cast<std::underlying_type_t<T>>(l._value) ^ static_cast<std::underlying_type_t<T>>(r._value));
    }

    friend constexpr EnumFlags operator^(EnumFlags l, T r) noexcept {
        return static_cast<T>(static_cast<std::underlying_type_t<T>>(l._value) ^ static_cast<std::underlying_type_t<T>>(r));
    }

    friend constexpr EnumFlags operator^(T l, EnumFlags r) noexcept {
        return static_cast<T>(static_cast<std::underlying_type_t<T>>(l) ^ static_cast<std::underlying_type_t<T>>(r._value));
    }

    friend constexpr EnumFlags operator~(EnumFlags v) noexcept {
        return static_cast<T>(~static_cast<std::underlying_type_t<T>>(v._value));
    }

    constexpr EnumFlags& operator|=(EnumFlags v) noexcept {
        _value = static_cast<T>(static_cast<std::underlying_type_t<T>>(_value) | static_cast<std::underlying_type_t<T>>(v._value));
        return *this;
    }

    constexpr EnumFlags& operator|=(T v) noexcept {
        _value = static_cast<T>(static_cast<std::underlying_type_t<T>>(_value) | static_cast<std::underlying_type_t<T>>(v));
        return *this;
    }

    constexpr EnumFlags& operator&=(EnumFlags v) noexcept {
        _value = static_cast<T>(static_cast<std::underlying_type_t<T>>(_value) & static_cast<std::underlying_type_t<T>>(v._value));
        return *this;
    }

    constexpr EnumFlags& operator&=(T v) noexcept {
        _value = static_cast<T>(static_cast<std::underlying_type_t<T>>(_value) & static_cast<std::underlying_type_t<T>>(v));
        return *this;
    }

    constexpr EnumFlags& operator^=(EnumFlags v) noexcept {
        _value = static_cast<T>(static_cast<std::underlying_type_t<T>>(_value) ^ static_cast<std::underlying_type_t<T>>(v._value));
        return *this;
    }

    constexpr EnumFlags& operator^=(T v) noexcept {
        _value = static_cast<T>(static_cast<std::underlying_type_t<T>>(_value) ^ static_cast<std::underlying_type_t<T>>(v));
        return *this;
    }

    string FormatByName() const
    requires(!is_compound_enum_flags<T>::value || enum_has_adl_format_as<T>)
    {
        if (this->value() == 0) {
            return "[]";
        }
        if constexpr (is_compound_enum_flags<T>::value) {
            return fmt::format("[{}]", format_as(_value));
        } else {
            const string names = EnumFlagsName(_value);
            if (names.empty()) {
                return FormatAsBits();
            }

            string result;
            result.reserve(names.size() + 2);
            result.push_back('[');
            for (char ch : names) {
                if (ch == '|') {
                    result.append(" | ");
                } else {
                    result.push_back(ch);
                }
            }
            result.push_back(']');
            return result;
        }
    }

    string FormatAsBits() const {
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
    if (value == 0) {
        return "[]";
    }
    if constexpr (
        !is_compound_enum_flags<T>::value ||
        enum_has_adl_format_as<T>) {
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
