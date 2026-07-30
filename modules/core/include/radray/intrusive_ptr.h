#pragma once

#include <compare>
#include <functional>
#include <type_traits>
#include <utility>

#include <radray/types.h>

namespace radray {

namespace detail {

struct IntrusiveAdoptTag {};
struct IntrusiveRetainTag {};

}  // namespace detail

template <class T>
concept IntrusiveRefCountable = requires(T* ptr) {
    IntrusivePtrAddRef(ptr);
    IntrusivePtrRelease(ptr);
};

template <class T>
requires IntrusiveRefCountable<T>
class IntrusivePtr;

template <class T>
requires IntrusiveRefCountable<T>
class IntrusivePtr {
public:
    using element_type = T;

    constexpr IntrusivePtr() noexcept = default;
    constexpr IntrusivePtr(std::nullptr_t) noexcept {}

    IntrusivePtr(T* ptr, detail::IntrusiveAdoptTag) noexcept : _ptr(ptr) {}

    IntrusivePtr(T* ptr, detail::IntrusiveRetainTag) noexcept : _ptr(ptr) {
        if (_ptr != nullptr) {
            IntrusivePtrAddRef(_ptr);
        }
    }

    IntrusivePtr(const IntrusivePtr& other) noexcept : _ptr(other._ptr) {
        if (_ptr != nullptr) {
            IntrusivePtrAddRef(_ptr);
        }
    }

    IntrusivePtr(IntrusivePtr&& other) noexcept : _ptr(other._ptr) {
        other._ptr = nullptr;
    }

    template <class U>
    requires(!std::is_same_v<U, T>) && std::is_convertible_v<U*, T*>
    IntrusivePtr(const IntrusivePtr<U>& other) noexcept : _ptr(other.Get()) {
        if (_ptr != nullptr) {
            IntrusivePtrAddRef(_ptr);
        }
    }

    template <class U>
    requires(!std::is_same_v<U, T>) && std::is_convertible_v<U*, T*>
    IntrusivePtr(IntrusivePtr<U>&& other) noexcept : _ptr(other.Release()) {}

    IntrusivePtr& operator=(const IntrusivePtr& other) noexcept {
        IntrusivePtr(other).Swap(*this);
        return *this;
    }

    IntrusivePtr& operator=(IntrusivePtr&& other) noexcept {
        IntrusivePtr(std::move(other)).Swap(*this);
        return *this;
    }

    IntrusivePtr& operator=(std::nullptr_t) noexcept {
        Reset();
        return *this;
    }

    ~IntrusivePtr() noexcept { Reset(); }

    T* Get() const noexcept { return _ptr; }
    T* operator->() const noexcept { return _ptr; }
    T& operator*() const noexcept { return *_ptr; }

    bool HasValue() const noexcept { return _ptr != nullptr; }
    explicit operator bool() const noexcept { return HasValue(); }

    void Reset() noexcept {
        if (T* old = std::exchange(_ptr, nullptr); old != nullptr) {
            IntrusivePtrRelease(old);
        }
    }

    [[nodiscard]] T* Release() noexcept { return std::exchange(_ptr, nullptr); }

    void Swap(IntrusivePtr& other) noexcept { std::swap(_ptr, other._ptr); }

    friend void swap(IntrusivePtr& lhs, IntrusivePtr& rhs) noexcept { lhs.Swap(rhs); }

    friend bool operator==(const IntrusivePtr& lhs, const IntrusivePtr& rhs) noexcept {
        return lhs._ptr == rhs._ptr;
    }

    friend std::strong_ordering operator<=>(const IntrusivePtr& lhs, const IntrusivePtr& rhs) noexcept {
        return std::compare_three_way{}(lhs._ptr, rhs._ptr);
    }

    friend bool operator==(const IntrusivePtr& lhs, std::nullptr_t) noexcept {
        return lhs._ptr == nullptr;
    }

private:
    T* _ptr{nullptr};
};

template <class T>
requires IntrusiveRefCountable<T>
[[nodiscard]] IntrusivePtr<T> AdoptRef(T* ptr) noexcept {
    return IntrusivePtr<T>(ptr, detail::IntrusiveAdoptTag{});
}

template <class T>
requires IntrusiveRefCountable<T>
[[nodiscard]] IntrusivePtr<T> AdoptRef(unique_ptr<T> ptr) noexcept {
    return IntrusivePtr<T>(ptr.release(), detail::IntrusiveAdoptTag{});
}

template <class T>
requires IntrusiveRefCountable<T>
[[nodiscard]] IntrusivePtr<T> RetainRef(T* ptr) noexcept {
    return IntrusivePtr<T>(ptr, detail::IntrusiveRetainTag{});
}

}  // namespace radray

namespace std {

template <class T>
struct hash<radray::IntrusivePtr<T>> {
    size_t operator()(const radray::IntrusivePtr<T>& ptr) const noexcept {
        return hash<T*>{}(ptr.Get());
    }
};

}  // namespace std
