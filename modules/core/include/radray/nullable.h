#pragma once

#include <radray/types.h>
#include <radray/logger.h>

namespace radray {

template <typename T>
requires requires(T v) {
    { v == nullptr } -> std::convertible_to<bool>;
    { v != nullptr } -> std::convertible_to<bool>;
}
struct Nullable {
    T Ptr{nullptr};

    constexpr Nullable() noexcept = default;
    constexpr Nullable(std::nullptr_t) noexcept : Ptr(nullptr) {}
    constexpr Nullable(T ptr) noexcept : Ptr(std::move(ptr)) {}
    template <typename U>
    requires std::convertible_to<U, T>
    constexpr Nullable(U&& ptr) noexcept : Ptr(std::forward<U>(ptr)) {}
    template <typename U>
    requires std::convertible_to<U, T>
    constexpr Nullable(const Nullable<U>& other) noexcept : Ptr(other.Ptr) {}

    constexpr Nullable(const Nullable& other) noexcept = default;
    constexpr Nullable(Nullable&& other) noexcept = default;
    constexpr Nullable& operator=(const Nullable& other) noexcept = default;
    constexpr Nullable& operator=(Nullable&& other) noexcept = default;

    template <typename U>
    requires std::convertible_to<U, T>
    constexpr Nullable& operator=(const Nullable<U>& other) noexcept {
        Ptr = other.Ptr;
        return *this;
    }
    template <typename U>
    requires std::convertible_to<U, T>
    constexpr Nullable& operator=(Nullable<U>&& other) noexcept {
        Ptr = std::move(other.Ptr);
        return *this;
    }

    constexpr bool HasValue() const noexcept { return Ptr != nullptr; }
    constexpr auto Value() const noexcept { return Ptr; }
    constexpr auto Unwrap() const noexcept {
        RADRAY_ASSERT(HasValue());
        return Ptr;
    }
    constexpr auto Release() noexcept {
        T tmp = Ptr;
        Ptr = nullptr;
        return tmp;
    }
    constexpr auto operator->() const noexcept {
        RADRAY_ASSERT(HasValue());
        return Ptr;
    }
    // decltype(auto) 可以保持引用，避免产生复制行为
    constexpr decltype(auto) operator*() const noexcept {
        RADRAY_ASSERT(HasValue());
        return *Ptr;
    }
};

template <typename T, typename D>
struct Nullable<std::unique_ptr<T, D>> {
    std::unique_ptr<T, D> Ptr;

    constexpr Nullable() noexcept = default;
    constexpr Nullable(std::nullptr_t) noexcept : Ptr{} {}
    constexpr Nullable(std::unique_ptr<T, D> ptr) noexcept : Ptr(std::move(ptr)) {}
    template <typename U, typename X>
    requires std::is_convertible_v<U*, T*> && std::conditional_t<std::is_reference_v<X>, std::is_same<X, D>, std::is_convertible<X, D>>::value
    constexpr Nullable(std::unique_ptr<U, X> ptr) noexcept : Ptr(std::move(ptr)) {}
    template <typename U, typename X>
    requires std::is_convertible_v<U*, T*> && std::conditional_t<std::is_reference_v<X>, std::is_same<X, D>, std::is_convertible<X, D>>::value
    constexpr Nullable(Nullable<std::unique_ptr<U, X>>&& other) noexcept : Ptr(std::move(other.Ptr)) {}

    constexpr Nullable(const Nullable&) = delete;
    constexpr Nullable(Nullable&&) = default;
    constexpr Nullable& operator=(const Nullable&) = delete;
    constexpr Nullable& operator=(Nullable&&) = default;

    template <typename U, typename X>
    requires std::is_convertible_v<U*, T*> && std::conditional_t<std::is_reference_v<X>, std::is_same<X, D>, std::is_convertible<X, D>>::value
    constexpr Nullable& operator=(std::unique_ptr<U, X> other) noexcept {
        Ptr = std::move(other);
        return *this;
    }
    template <typename U, typename X>
    requires std::is_convertible_v<U*, T*> && std::conditional_t<std::is_reference_v<X>, std::is_same<X, D>, std::is_convertible<X, D>>::value
    constexpr Nullable& operator=(Nullable<std::unique_ptr<U, X>>&& other) noexcept {
        Ptr = std::move(other.Ptr);
        return *this;
    }

    constexpr bool HasValue() const noexcept { return Ptr != nullptr; }
    constexpr auto Value() const noexcept { return Ptr.get(); }
    constexpr auto Unwrap() noexcept {
        RADRAY_ASSERT(HasValue());
        return std::move(Ptr);
    }
    constexpr auto Release() noexcept {
        return std::move(Ptr);
    }
    constexpr auto operator->() const noexcept {
        RADRAY_ASSERT(HasValue());
        return Ptr.get();
    }
    constexpr decltype(auto) operator*() const noexcept {
        RADRAY_ASSERT(HasValue());
        return *Ptr;
    }
};
template <typename T, typename D, typename U, typename E>
constexpr bool operator==(const Nullable<std::unique_ptr<T, D>>& lhs, const Nullable<std::unique_ptr<U, E>>& rhs) noexcept {
    return lhs.Ptr.get() == rhs.Ptr.get();
}
template <typename T, typename D, typename U, typename E>
constexpr bool operator!=(const Nullable<std::unique_ptr<T, D>>& lhs, const Nullable<std::unique_ptr<U, E>>& rhs) noexcept {
    return lhs.Ptr.get() != rhs.Ptr.get();
}

template <typename T>
struct Nullable<std::shared_ptr<T>> {
    std::shared_ptr<T> Ptr;

    constexpr Nullable() noexcept = default;
    constexpr Nullable(std::nullptr_t) noexcept : Ptr{} {}
    constexpr Nullable(std::shared_ptr<T> ptr) noexcept : Ptr(std::move(ptr)) {}
    template <typename U>
    constexpr Nullable(std::shared_ptr<U> ptr) noexcept : Ptr(std::move(ptr)) {}
    template <typename U>
    constexpr Nullable(Nullable<U>&& other) noexcept : Ptr(std::move(other.Ptr)) {}

    constexpr Nullable(const Nullable&) = default;
    constexpr Nullable(Nullable&&) = default;
    constexpr Nullable& operator=(const Nullable&) = default;
    constexpr Nullable& operator=(Nullable&&) = default;

    template <typename U>
    constexpr Nullable& operator=(std::shared_ptr<U> other) noexcept {
        Ptr = std::move(other);
        return *this;
    }
    template <typename U>
    constexpr Nullable& operator=(Nullable<std::shared_ptr<U>> other) noexcept {
        Ptr = std::move(other.Ptr);
        return *this;
    }

    constexpr bool HasValue() const noexcept { return Ptr != nullptr; }
    constexpr auto Value() const noexcept { return Ptr.get(); }
    constexpr auto Unwrap() noexcept {
        RADRAY_ASSERT(HasValue());
        return std::move(Ptr);
    }
    constexpr auto Release() noexcept {
        std::shared_ptr<T> tmp = std::move(Ptr);
        return tmp;
    }
    constexpr auto operator->() const noexcept {
        RADRAY_ASSERT(HasValue());
        return Ptr.get();
    }
    constexpr decltype(auto) operator*() const noexcept {
        RADRAY_ASSERT(HasValue());
        return *Ptr;
    }
};

namespace detail {
template <typename N>
constexpr auto NullableAddress(const N& n) noexcept {
    // n 必须有成员 Ptr
    if constexpr (requires { n.Ptr.get(); }) {
        // unique_ptr / shared_ptr
        return n.Ptr.get();
    } else {
        // 原始指针 (或已是裸指针语义)
        return n.Ptr;
    }
}
}  // namespace detail

// 统一 Nullable 与 Nullable
template <typename A, typename B>
constexpr bool operator==(const Nullable<A>& lhs, const Nullable<B>& rhs) noexcept {
    return detail::NullableAddress(lhs) == detail::NullableAddress(rhs);
}
template <typename A, typename B>
constexpr bool operator!=(const Nullable<A>& lhs, const Nullable<B>& rhs) noexcept {
    return detail::NullableAddress(lhs) != detail::NullableAddress(rhs);
}

// 与 nullptr
template <typename A>
constexpr bool operator==(const Nullable<A>& n, std::nullptr_t) noexcept {
    return !n.HasValue();
}
template <typename A>
constexpr bool operator==(std::nullptr_t, const Nullable<A>& n) noexcept {
    return !n.HasValue();
}
template <typename A>
constexpr bool operator!=(const Nullable<A>& n, std::nullptr_t) noexcept {
    return n.HasValue();
}
template <typename A>
constexpr bool operator!=(std::nullptr_t, const Nullable<A>& n) noexcept {
    return n.HasValue();
}

}  // namespace radray
