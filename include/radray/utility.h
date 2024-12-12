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
