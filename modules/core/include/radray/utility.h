#pragma once

#include <utility>
#include <filesystem>
#include <optional>
#include <string_view>
#include <type_traits>
#include <span>
#include <iterator>
#include <concepts>
#include <functional>

#include <sigslot/signal.hpp>

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

template <typename T>
struct FunctionTraits;
template <typename Ret, typename... Args>
struct FunctionTraits<Ret(Args...)> {
    using ReturnType = Ret;
    using ArgsTuple = std::tuple<Args...>;
    static constexpr std::size_t ArgsCount = sizeof...(Args);
};
template <typename Ret, typename... Args>
struct FunctionTraits<Ret (*)(Args...)> : FunctionTraits<Ret(Args...)> {};
template <typename Ret, typename... Args>
struct FunctionTraits<std::function<Ret(Args...)>> : FunctionTraits<Ret(Args...)> {};
template <typename ClassType, typename Ret, typename... Args>
struct FunctionTraits<Ret (ClassType::*)(Args...)> : FunctionTraits<Ret(Args...)> {};
template <typename ClassType, typename Ret, typename... Args>
struct FunctionTraits<Ret (ClassType::*)(Args...) const> : FunctionTraits<Ret(Args...)> {};

// Resolves to the more efficient of `const T` or `const T&`, in the context of returning a const-qualified value
// of type T.
//
// Copied from cppfront's implementation of the CppCoreGuidelines F.16 (https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#Rf-in)
template <typename T>
using value_or_reference_return_t = std::conditional_t<
    sizeof(T) <= 2 * sizeof(void*) && std::is_trivially_copy_constructible<T>::value,
    const T,
    const T&>;

class Noncopyable {
protected:
    constexpr Noncopyable() = default;
    ~Noncopyable() = default;

    Noncopyable(const Noncopyable&) = delete;
    Noncopyable& operator=(const Noncopyable&) = delete;
};

struct StringHash {
    using hash_type = std::hash<std::string_view>;
    using is_transparent = void;

    size_t operator()(const char* str) const noexcept { return hash_type{}(str); }
    size_t operator()(std::string_view str) const noexcept { return hash_type{}(str); }
    template <class Char, class Traits, class Alloc>
    size_t operator()(std::basic_string<Char, Traits, Alloc> const& str) const noexcept { return hash_type{}(str); }
};

std::optional<string> ReadText(const std::filesystem::path& filepath) noexcept;

std::optional<wstring> ToWideChar(std::string_view str) noexcept;

std::optional<string> ToMultiByte(std::wstring_view str) noexcept;

vector<uint32_t> ByteToDWORD(std::span<uint8_t> bytes) noexcept;

[[noreturn]] inline void Unreachable() noexcept {
#ifdef __cpp_lib_unreachable
    std::unreachable();
#else
#if defined(_MSC_VER)
    __assume(false);
#elif defined(__clang__) || defined(__GNUC__)
    __builtin_unreachable();
#else
#error "Unreachable not supported on this compiler"
#endif
#endif
}

size_t HashData(const void* data, size_t size) noexcept;

}  // namespace radray
