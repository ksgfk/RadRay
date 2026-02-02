#pragma once

#include <span>
#include <concepts>
#include <memory>

#include <sigslot/signal.hpp>

#include <radray/types.h>

#define RADRAY_UNUSED(expr) \
    do {                    \
        (void)(expr);       \
    } while (0)

namespace radray {

template <class To, class From>
requires std::is_same_v<typename std::unique_ptr<From>::deleter_type, std::default_delete<From>>
[[nodiscard]] constexpr std::unique_ptr<To> StaticCastUniquePtr(std::unique_ptr<From>&& from) noexcept {
    return std::unique_ptr<To>(static_cast<To*>(from.release()));
}

vector<uint32_t> ByteToDWORD(std::span<const uint8_t> bytes) noexcept;

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

}  // namespace radray
