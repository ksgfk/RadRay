#pragma once

#include <utility>
#include <type_traits>

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

template <class T, size_t Length>
requires std::is_copy_assignable_v<T>
constexpr void CopyAssign(T (&dst)[Length], T (&src)[Length]) {
    for (size_t i = 0; i < Length; i++) {
        dst[i] = src[i];
    }
}

}  // namespace radray
