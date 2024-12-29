#pragma once

#include <utility>
#include <filesystem>
#include <optional>
#include <string_view>
#include <type_traits>
#include <span>
#include <iterator>
#include <concepts>

#include <radray/types.h>
#include <radray/logger.h>

#define RADRAY_UNUSED(expr) \
    do {                    \
        (void)(expr);       \
    } while (0)

namespace radray {

template <typename Call>
class ScopeGuard {
public:
    explicit constexpr ScopeGuard(Call&& f) noexcept : _fun(std::forward<Call>(f)), _active(true) {}
    constexpr ScopeGuard(ScopeGuard&& rhs) noexcept : _fun(std::move(rhs._fun)), _active(rhs._active) {
        rhs.Dismiss();
    }
    constexpr ~ScopeGuard() noexcept {
        if (_active) {
            _fun();
        }
    }
    ScopeGuard() = delete;
    ScopeGuard(const ScopeGuard&) = delete;
    ScopeGuard& operator=(const ScopeGuard&) = delete;

    constexpr void Dismiss() noexcept { _active = false; }

private:
    Call _fun;
    bool _active;
};

template <typename Call>
constexpr auto MakeScopeGuard(Call&& f) noexcept { return ScopeGuard<Call>{std::forward<Call>(f)}; }

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
    template <typename U>
    constexpr Nullable(std::unique_ptr<U, D> ptr) noexcept : Ptr(std::move(ptr)) {}
    template <typename U>
    constexpr Nullable& operator=(std::unique_ptr<U, D> other) noexcept {
        Ptr = std::move(other.Ptr);
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
        Ptr = other.Ptr;
        return *this;
    }
    template <typename U>
    constexpr Nullable(Nullable<U>&& other) noexcept : Ptr(other.Ptr) {}
    template <typename U>
    constexpr Nullable& operator=(Nullable<U> other) noexcept {
        Ptr = other.Ptr;
        return *this;
    }

    constexpr bool HasValue() const noexcept { return Ptr != nullptr; }
    constexpr T* Value() const noexcept { return Ptr.get(); }
    constexpr std::shared_ptr<T>&& Unwrap() noexcept {
        RADRAY_ASSERT(HasValue());
        return std::move(Ptr);
    }
    constexpr T* GetValueOrDefault(T* defaultPtr) const noexcept { return HasValue() ? Ptr.get() : defaultPtr; }
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
struct ArrayTrait;
template <typename T, size_t N>
struct ArrayTrait<T[N]> {
    static constexpr size_t length = N;
};

template <typename T>
requires(std::is_bounded_array_v<T>)
constexpr auto ArrayLength(const T& arr) noexcept {
    RADRAY_UNUSED(arr);
    return ArrayTrait<T>::length;
}

template <typename Iter>
concept IsIterator = requires(Iter ite, size_t n) {
    { *ite };
    { !std::is_integral_v<Iter> };
    { std::distance(ite, ite) } -> std::same_as<typename std::iterator_traits<Iter>::difference_type>;
    { std::advance(ite, n) };
};

class Noncopyable {
protected:
    constexpr Noncopyable() = default;
    ~Noncopyable() = default;

    Noncopyable(const Noncopyable&) = delete;
    Noncopyable& operator=(const Noncopyable&) = delete;
};

std::optional<radray::string> ReadText(const std::filesystem::path& filepath) noexcept;

std::optional<radray::wstring> ToWideChar(std::string_view str) noexcept;

std::optional<radray::string> ToMultiByte(std::wstring_view str) noexcept;

radray::vector<uint32_t> ByteToDWORD(std::span<uint8_t> bytes) noexcept;

[[noreturn]] inline void Unreachable() noexcept {
#ifdef __cpp_lib_unreachable
    std::unreachable();
#else
#if defined(_MSC_VER)
    __assume(false);
#elif defined(__clang__) || defined(__GNUC__)
    __builtin_unreachable();
#else
    // no impl
#endif
#endif
}

}  // namespace radray
