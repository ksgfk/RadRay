#pragma once

#include <concepts>
#include <utility>
#include <type_traits>
#include <source_location>
#include <stdexcept>

#include <radray/logger.h>
#include <radray/utility.h>

namespace radray {

class NullableAccessException : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

template <class T>
requires std::is_convertible_v<decltype(std::declval<T>() != nullptr), bool> && std::is_constructible_v<T, std::nullptr_t>
class Nullable {
public:
    constexpr Nullable() noexcept(std::is_nothrow_default_constructible_v<T>) = default;

    constexpr ~Nullable() noexcept = default;

    constexpr Nullable(std::nullptr_t) noexcept(std::is_nothrow_constructible_v<T, std::nullptr_t>) : _value{nullptr} {}

    template <class U>
    requires std::is_convertible_v<U, T>
    constexpr Nullable(U&& u) noexcept(std::is_nothrow_move_constructible_v<T>) : _value(std::forward<U>(u)) {}

    constexpr Nullable(T u) noexcept(std::is_nothrow_move_constructible_v<T>) : _value(std::move(u)) {}

    template <class U>
    requires std::is_convertible_v<U, T>
    constexpr Nullable(const Nullable<U>& other) noexcept(std::is_nothrow_copy_constructible_v<T>) : _value(other._value) {}

    template <class U>
    requires std::is_convertible_v<U, T>
    constexpr Nullable(Nullable<U>&& other) noexcept(std::is_nothrow_move_constructible_v<T>) : _value(std::move(other._value)) {}

    constexpr Nullable(const Nullable& other) noexcept(std::is_nothrow_copy_constructible_v<T>) = default;
    constexpr Nullable& operator=(const Nullable& other) noexcept(std::is_nothrow_copy_assignable_v<T>) = default;

    constexpr Nullable(Nullable&& other) noexcept(std::is_nothrow_move_constructible_v<T>) = default;
    constexpr Nullable& operator=(Nullable&& other) noexcept(std::is_nothrow_move_assignable_v<T>) = default;

    constexpr value_or_reference_return_t<T> Get() const noexcept(noexcept(value_or_reference_return_t<T>(std::declval<T&>()))) {
        return _value;
    }

    constexpr bool HasValue() const noexcept { return _value != nullptr; }

    constexpr explicit operator bool() const noexcept { return HasValue(); }

    constexpr T Unwrap(std::source_location location = std::source_location::current()) & {
        if (!HasValue()) [[unlikely]] {
            throw NullableAccessException(fmt::format("NullableAccessException\n  at {}:{}", location.file_name(), location.line()));
        }
        return _value;
    }

    constexpr T Unwrap(std::source_location location = std::source_location::current()) && {
        if (!HasValue()) [[unlikely]] {
            throw NullableAccessException(fmt::format("NullableAccessException\n  at {}:{}", location.file_name(), location.line()));
        }
        return std::move(_value);
    }

    constexpr T Release() & noexcept { return std::exchange(_value, nullptr); }

    constexpr T Release() && noexcept { return std::move(_value); }

    constexpr decltype(auto) operator->() const noexcept { return Get(); }

    constexpr decltype(auto) operator*() const noexcept { return *Get(); }

public:
    T _value;
};

template <class T, class TDeleter>
class Nullable<std::unique_ptr<T, TDeleter>> {
public:
    constexpr Nullable() noexcept = default;

    constexpr ~Nullable() noexcept = default;

    constexpr Nullable(std::nullptr_t) noexcept : _value{nullptr} {}

    template <class U, class UDeleter>
    requires std::is_convertible_v<U*, T*> && std::conditional_t<std::is_reference_v<UDeleter>, std::is_same<UDeleter, TDeleter>, std::is_convertible<UDeleter, TDeleter>>::value
    constexpr Nullable(std::unique_ptr<U, UDeleter> ptr) noexcept : _value(std::move(ptr)) {}

    template <class U, class UDeleter>
    requires std::is_convertible_v<U*, T*> && std::conditional_t<std::is_reference_v<UDeleter>, std::is_same<UDeleter, TDeleter>, std::is_convertible<UDeleter, TDeleter>>::value
    constexpr Nullable(Nullable<std::unique_ptr<U, UDeleter>> other) noexcept : _value(std::move(other._value)) {}

    constexpr Nullable(const Nullable& other) noexcept = default;
    constexpr Nullable& operator=(const Nullable& other) noexcept = default;

    constexpr Nullable(Nullable&& other) noexcept = default;
    constexpr Nullable& operator=(Nullable&& other) noexcept = default;

    constexpr T* Get() const noexcept { return _value.get(); }

    constexpr bool HasValue() const noexcept { return static_cast<bool>(_value); }

    constexpr explicit operator bool() const noexcept { return static_cast<bool>(_value); }

    constexpr std::unique_ptr<T, TDeleter> Unwrap(std::source_location location = std::source_location::current()) {
        if (!HasValue()) [[unlikely]] {
            throw NullableAccessException(fmt::format("NullableAccessException\n  at {}:{}", location.file_name(), location.line()));
        }
        return std::move(_value);
    }

    constexpr std::unique_ptr<T, TDeleter> Release() noexcept { return std::move(_value); }

    constexpr decltype(auto) operator->() const noexcept { return Get(); }

    constexpr decltype(auto) operator*() const noexcept { return *Get(); }

public:
    std::unique_ptr<T, TDeleter> _value;
};

template <class T>
class Nullable<std::shared_ptr<T>> {
public:
    constexpr Nullable() noexcept = default;

    constexpr ~Nullable() noexcept = default;

    constexpr Nullable(std::nullptr_t) noexcept : _value{nullptr} {}

    template <class U>
    requires std::is_convertible_v<U*, T*>
    constexpr Nullable(const std::shared_ptr<U>& ptr) noexcept : _value(ptr) {}

    template <class U>
    requires std::is_convertible_v<U*, T*>
    constexpr Nullable(std::shared_ptr<U>&& ptr) noexcept : _value(std::move(ptr)) {}

    template <class U>
    requires std::is_convertible_v<U*, T*>
    constexpr Nullable(const Nullable<std::shared_ptr<U>>& other) noexcept : _value(other._value) {}

    template <class U>
    requires std::is_convertible_v<U*, T*>
    constexpr Nullable(Nullable<std::shared_ptr<U>>&& other) noexcept : _value(std::move(other._value)) {}

    constexpr Nullable(const Nullable& other) noexcept = default;
    constexpr Nullable& operator=(const Nullable& other) noexcept = default;

    constexpr Nullable(Nullable&& other) noexcept = default;
    constexpr Nullable& operator=(Nullable&& other) noexcept = default;

    constexpr T* Get() const noexcept { return _value.get(); }

    constexpr bool HasValue() const noexcept { return static_cast<bool>(_value); }

    constexpr explicit operator bool() const noexcept { return static_cast<bool>(_value); }

    constexpr std::shared_ptr<T> Unwrap(std::source_location location = std::source_location::current()) {
        if (!HasValue()) [[unlikely]] {
            throw NullableAccessException(fmt::format("NullableAccessException\n  at {}:{}", location.file_name(), location.line()));
        }
        return std::move(_value);
    }

    constexpr std::shared_ptr<T> Release() noexcept { return std::move(_value); }

    constexpr decltype(auto) operator->() const noexcept { return Get(); }

    constexpr decltype(auto) operator*() const noexcept { return *Get(); }

public:
    std::shared_ptr<T> _value;
};

template <class T, class U>
constexpr bool operator==(const Nullable<T>& lhs, const Nullable<U>& rhs) noexcept {
    return lhs._value == rhs._value;
}

template <class T, class U>
constexpr bool operator!=(const Nullable<T>& lhs, const Nullable<U>& rhs) noexcept {
    return lhs._value != rhs._value;
}

template <class T>
constexpr bool operator==(const Nullable<T>& lhs, std::nullptr_t) noexcept {
    return lhs._value == nullptr;
}

template <class T>
constexpr bool operator!=(const Nullable<T>& lhs, std::nullptr_t) noexcept {
    return lhs._value != nullptr;
}

template <class T>
constexpr bool operator==(std::nullptr_t, const Nullable<T>& rhs) noexcept {
    return rhs._value == nullptr;
}

template <class T>
constexpr bool operator!=(std::nullptr_t, const Nullable<T>& rhs) noexcept {
    return rhs._value != nullptr;
}

}  // namespace radray
