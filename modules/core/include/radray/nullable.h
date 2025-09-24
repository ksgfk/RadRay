#pragma once

#include <radray/types.h>
#include <radray/logger.h>

namespace radray {

template <typename T>
struct Nullable {
    T* Ptr{nullptr};

    constexpr Nullable() noexcept = default;
    constexpr Nullable(std::nullptr_t) noexcept : Ptr(nullptr) {}
    constexpr Nullable(T* ptr) noexcept : Ptr(ptr) {}
    template <typename U>
    requires std::convertible_to<U*, T*>
    constexpr Nullable(U* ptr) noexcept : Ptr(ptr) {}
    constexpr Nullable(const Nullable& other) noexcept = default;
    constexpr Nullable(Nullable&& other) noexcept = default;
    constexpr Nullable& operator=(const Nullable& other) noexcept = default;
    constexpr Nullable& operator=(Nullable&& other) noexcept = default;
    template <typename U>
    requires std::convertible_to<U*, T*>
    constexpr Nullable(const Nullable<U>& other) noexcept : Ptr(other.Ptr) {}
    template <typename U>
    requires std::convertible_to<U*, T*>
    constexpr Nullable(Nullable<U>&& other) noexcept : Ptr(other.Ptr) {}
    template <typename U>
    requires std::convertible_to<U*, T*>
    constexpr Nullable& operator=(const Nullable<U>& other) noexcept {
        Ptr = other.Ptr;
        return *this;
    }
    template <typename U>
    requires std::convertible_to<U*, T*>
    constexpr Nullable& operator=(Nullable<U>&& other) noexcept {
        Ptr = other.Ptr;
        return *this;
    }

    constexpr bool HasValue() const noexcept { return Ptr != nullptr; }
    constexpr T* Value() const noexcept { return Ptr; }
    constexpr T* Unwrap() const noexcept {
        RADRAY_ASSERT(HasValue());
        return Ptr;
    }
    constexpr T* GetValueOrDefault(T* defaultPtr) const noexcept { return HasValue() ? Ptr : defaultPtr; }
    constexpr T* Release() noexcept {
        T* tmp = Ptr;
        Ptr = nullptr;
        return tmp;
    }
    constexpr T* operator->() const noexcept {
        RADRAY_ASSERT(HasValue());
        return Ptr;
    }
    constexpr T& operator*() const noexcept {
        RADRAY_ASSERT(HasValue());
        return *Ptr;
    }
    constexpr operator bool() const noexcept { return HasValue(); }
};

template <typename T>
struct Nullable<T*> {
    T* Ptr{nullptr};

    constexpr Nullable() noexcept = default;
    constexpr Nullable(std::nullptr_t) noexcept : Ptr(nullptr) {}
    constexpr Nullable(T* ptr) noexcept : Ptr(ptr) {}
    template <typename U>
    requires std::convertible_to<U*, T*>
    constexpr Nullable(U* ptr) noexcept : Ptr(ptr) {}
    constexpr Nullable(const Nullable& other) noexcept = default;
    constexpr Nullable(Nullable&& other) noexcept = default;
    constexpr Nullable& operator=(const Nullable& other) noexcept = default;
    constexpr Nullable& operator=(Nullable&& other) noexcept = default;
    template <typename U>
    requires std::convertible_to<U*, T*>
    constexpr Nullable(const Nullable<U>& other) noexcept : Ptr(other.Ptr) {}
    template <typename U>
    requires std::convertible_to<U*, T*>
    constexpr Nullable(Nullable<U>&& other) noexcept : Ptr(other.Ptr) {}
    template <typename U>
    requires std::convertible_to<U*, T*>
    constexpr Nullable& operator=(const Nullable<U>& other) noexcept {
        Ptr = other.Ptr;
        return *this;
    }
    template <typename U>
    requires std::convertible_to<U*, T*>
    constexpr Nullable& operator=(Nullable<U>&& other) noexcept {
        Ptr = other.Ptr;
        return *this;
    }

    constexpr bool HasValue() const noexcept { return Ptr != nullptr; }
    constexpr T* Value() const noexcept { return Ptr; }
    constexpr T* Unwrap() const noexcept {
        RADRAY_ASSERT(HasValue());
        return Ptr;
    }
    constexpr T* GetValueOrDefault(T* defaultPtr) const noexcept { return HasValue() ? Ptr : defaultPtr; }
    constexpr T* Release() noexcept {
        T* tmp = Ptr;
        Ptr = nullptr;
        return tmp;
    }
    constexpr T* operator->() const noexcept {
        RADRAY_ASSERT(HasValue());
        return Ptr;
    }
    constexpr T& operator*() const noexcept {
        RADRAY_ASSERT(HasValue());
        return *Ptr;
    }
    constexpr operator bool() const noexcept { return HasValue(); }
};

template <>
struct Nullable<void> {
    void* Ptr{nullptr};

    constexpr Nullable() noexcept = default;
    constexpr Nullable(std::nullptr_t) noexcept : Ptr(nullptr) {}
    constexpr Nullable(void* ptr) noexcept : Ptr(ptr) {}
    template <typename U>
    constexpr Nullable(U* ptr) noexcept : Ptr(ptr) {}
    constexpr Nullable(const Nullable& other) noexcept = default;
    constexpr Nullable(Nullable&& other) noexcept = default;
    constexpr Nullable& operator=(const Nullable& other) noexcept = default;
    constexpr Nullable& operator=(Nullable&& other) noexcept = default;
    template <typename U>
    constexpr Nullable(const Nullable<U>& other) noexcept : Ptr(other.Ptr) {}
    template <typename U>
    constexpr Nullable(Nullable<U>&& other) noexcept : Ptr(other.Ptr) {}
    template <typename U>
    constexpr Nullable& operator=(const Nullable<U>& other) noexcept {
        Ptr = other.Ptr;
        return *this;
    }
    template <typename U>
    constexpr Nullable& operator=(Nullable<U>&& other) noexcept {
        Ptr = other.Ptr;
        return *this;
    }

    constexpr bool HasValue() const noexcept { return Ptr != nullptr; }
    constexpr void* Value() const noexcept { return Ptr; }
    constexpr void* Unwrap() const noexcept {
        RADRAY_ASSERT(HasValue());
        return Ptr;
    }
    constexpr void* GetValueOrDefault(void* defaultPtr) const noexcept { return HasValue() ? Ptr : defaultPtr; }
    constexpr void* Release() noexcept {
        void* tmp = Ptr;
        Ptr = nullptr;
        return tmp;
    }
    constexpr void* operator->() const noexcept {
        RADRAY_ASSERT(HasValue());
        return Ptr;
    }
    constexpr operator bool() const noexcept { return HasValue(); }
};

template <typename T, typename D>
struct Nullable<std::unique_ptr<T, D>> {
    std::unique_ptr<T, D> Ptr;

    constexpr Nullable() noexcept = default;
    constexpr Nullable(std::nullptr_t) noexcept : Ptr{} {}
    constexpr Nullable(std::unique_ptr<T, D> ptr) noexcept : Ptr(std::move(ptr)) {}
    constexpr Nullable(const Nullable&) = delete;
    constexpr Nullable(Nullable&&) = default;
    constexpr Nullable& operator=(const Nullable&) = delete;
    constexpr Nullable& operator=(Nullable&&) = default;
    template <typename U, typename X>
    constexpr Nullable(std::unique_ptr<U, X> ptr) noexcept : Ptr(std::move(ptr)) {}
    template <typename U, typename X>
    constexpr Nullable& operator=(std::unique_ptr<U, X> other) noexcept {
        Ptr = std::move(other);
        return *this;
    }
    template <typename U>
    constexpr Nullable(Nullable<U>&& other) noexcept : Ptr(std::move(other.Ptr)) {}
    template <typename U>
    constexpr Nullable& operator=(Nullable<U> other) noexcept {
        Ptr = std::move(other.Ptr);
        return *this;
    }

    constexpr bool HasValue() const noexcept { return Ptr != nullptr; }
    constexpr T* Value() const noexcept { return Ptr.get(); }
    constexpr std::unique_ptr<T, D>&& Unwrap() noexcept {
        RADRAY_ASSERT(HasValue());
        return std::move(Ptr);
    }
    constexpr T* GetValueOrDefault(T* defaultPtr) const noexcept { return HasValue() ? Ptr.get() : defaultPtr; }
    constexpr std::unique_ptr<T, D> Release() noexcept {
        return std::move(Ptr);
    }
    constexpr T* operator->() const noexcept {
        RADRAY_ASSERT(HasValue());
        return Ptr.get();
    }
    constexpr T& operator*() const noexcept {
        RADRAY_ASSERT(HasValue());
        return *Ptr;
    }
    constexpr operator bool() const noexcept { return HasValue(); }
};

template <typename T>
struct Nullable<std::shared_ptr<T>> {
    std::shared_ptr<T> Ptr;

    constexpr Nullable() noexcept = default;
    constexpr Nullable(std::nullptr_t) noexcept : Ptr{} {}
    constexpr Nullable(std::shared_ptr<T> ptr) noexcept : Ptr(std::move(ptr)) {}
    constexpr Nullable(const Nullable&) = default;
    constexpr Nullable(Nullable&&) = default;
    constexpr Nullable& operator=(const Nullable&) = default;
    constexpr Nullable& operator=(Nullable&&) = default;
    template <typename U>
    constexpr Nullable(std::shared_ptr<U> ptr) noexcept : Ptr(ptr) {}
    template <typename U>
    constexpr Nullable& operator=(std::shared_ptr<U> other) noexcept {
        Ptr = std::move(other);
        return *this;
    }
    template <typename U>
    constexpr Nullable(Nullable<U>&& other) noexcept : Ptr(other.Ptr) {}
    template <typename U>
    constexpr Nullable& operator=(Nullable<U> other) noexcept {
        Ptr = std::move(other.Ptr);
        return *this;
    }

    constexpr bool HasValue() const noexcept { return Ptr != nullptr; }
    constexpr T* Value() const noexcept { return Ptr.get(); }
    constexpr std::shared_ptr<T>&& Unwrap() noexcept {
        RADRAY_ASSERT(HasValue());
        return std::move(Ptr);
    }
    constexpr T* GetValueOrDefault(T* defaultPtr) const noexcept { return HasValue() ? Ptr.get() : defaultPtr; }
    constexpr std::shared_ptr<T> Release() noexcept {
        std::shared_ptr<T> tmp = std::move(Ptr);
        Ptr.reset();
        return tmp;
    }
    constexpr T* operator->() const noexcept {
        RADRAY_ASSERT(HasValue());
        return Ptr.get();
    }
    constexpr T& operator*() const noexcept {
        RADRAY_ASSERT(HasValue());
        return *Ptr;
    }
    constexpr operator bool() const noexcept { return HasValue(); }
};

}  // namespace radray
