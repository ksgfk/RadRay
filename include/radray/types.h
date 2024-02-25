#pragma once

#include <cstdint>
#include <cstddef>
#include <type_traits>
#include <tuple>

namespace radray {

using uint8 = std::uint8_t;
using int8 = std::int8_t;
using int16 = std::int16_t;
using uint16 = std::uint16_t;
using int32 = std::int32_t;
using uint32 = std::uint32_t;
using int64 = std::int64_t;
using uint64 = std::uint64_t;

template <typename T>
constexpr auto always_false_v = std::false_type::value;

template <typename T>
struct ArrayTrait;
template <typename T, size_t N>
struct ArrayTrait<T[N]> {
    static constexpr size_t Length = N;
    static constexpr size_t ByteSize = N * sizeof(T);
};
template <typename T>
requires(std::is_bounded_array_v<T>)
constexpr auto array_length_v = ArrayTrait<T>::Length;
template <typename T>
requires(std::is_bounded_array_v<T>)
constexpr auto ArrayLength(const T& arr) { return ArrayTrait<T>::Length; }
template <typename T>
requires(std::is_bounded_array_v<T>)
constexpr auto ArrayFirst(const T& arr) {
    static_assert(ArrayTrait<T>::Length != 0, "array length cannot be 0");
    return &arr[0];
}
template <typename T>
requires(std::is_bounded_array_v<T>)
constexpr auto ArrayLast(const T& arr) {
    constexpr auto length = ArrayTrait<T>::Length;
    static_assert(length != 0, "array length cannot be 0");
    return &arr[length - 1];
}

template <typename... Args>
struct has_rvalue_reference {
    static constexpr bool value = false;
};
template <typename T, typename... Args>
struct has_rvalue_reference<T, Args...> {
    static constexpr bool value = std::is_rvalue_reference<T>::value || has_rvalue_reference<Args...>::value;
};
template <typename... Args>
constexpr auto has_rvalue_reference_v = has_rvalue_reference<Args...>::value;

template <typename T>
struct remove_all_cvref {
    using type = std::remove_cvref_t<T>;
};
template <typename T>
struct remove_all_cvref<T*> {
    using type = typename remove_all_cvref<T>::type;
};
template <typename T>
struct remove_all_cvref<T&> {
    using type = typename remove_all_cvref<T>::type;
};
template <typename T>
struct remove_all_cvref<T&&> {
    using type = typename remove_all_cvref<T>::type;
};
template <class T>
using remove_all_cvref_t = typename remove_all_cvref<T>::type;

template <typename T>
requires std::is_function_v<T>
struct FunctionTrait;

template <typename Return, typename... Args>
struct CallableParameterTrait {
    static constexpr size_t ParameterCount = sizeof...(Args);
    using Parameters = std::tuple<Args...>;
    using ReturnType = Return;
    template <size_t N>
    struct Parameter {
        static_assert(N < ParameterCount, "index out of range");
        using type = typename std::tuple_element_t<N, std::tuple<Args...>>;
    };
};
template <typename T>
struct CallableTrait;
template <typename Type, typename Return, typename... Args>
struct CallableTrait<Return (Type::*)(Args...)> : public CallableParameterTrait<Return, Args...> {
    static constexpr bool IsMember = true;
    static constexpr bool IsConst = false;
    static constexpr bool IsNoExcept = false;
    static constexpr bool IsFunctor = false;
};
template <typename Type, typename Return, typename... Args>
struct CallableTrait<Return (Type::*)(Args...) noexcept> : public CallableParameterTrait<Return, Args...> {
    static constexpr bool IsMember = true;
    static constexpr bool IsConst = false;
    static constexpr bool IsNoExcept = true;
    static constexpr bool IsFunctor = false;
};
template <typename Type, typename Return, typename... Args>
struct CallableTrait<Return (Type::*)(Args...) const> : public CallableParameterTrait<Return, Args...> {
    static constexpr bool IsMember = true;
    static constexpr bool IsConst = true;
    static constexpr bool IsNoExcept = false;
    static constexpr bool IsFunctor = false;
};
template <typename Type, typename Return, typename... Args>
struct CallableTrait<Return (Type::*)(Args...) const noexcept> : public CallableParameterTrait<Return, Args...> {
    static constexpr bool IsMember = true;
    static constexpr bool IsConst = true;
    static constexpr bool IsNoExcept = true;
    static constexpr bool IsFunctor = false;
};
template <typename Type>
struct CallableTrait : public CallableTrait<decltype(&Type::operator())> {
    static constexpr bool IsFunctor = true;
};
template <typename Return, typename... Args>
struct CallableTrait<Return (*)(Args...)> : public CallableParameterTrait<Return, Args...> {
    static constexpr bool IsMember = false;
    static constexpr bool IsConst = false;
    static constexpr bool IsNoExcept = false;
    static constexpr bool IsFunctor = false;
};
template <typename Return, typename... Args>
struct CallableTrait<Return (*)(Args...) noexcept> : public CallableParameterTrait<Return, Args...> {
    static constexpr bool IsMember = false;
    static constexpr bool IsConst = false;
    static constexpr bool IsNoExcept = true;
    static constexpr bool IsFunctor = false;
};

}  // namespace radray
