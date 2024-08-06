#pragma once

#include <utility>
#include <memory_resource>

namespace radray {

template <typename Call>
class ScopeGuard {
public:
    explicit ScopeGuard(Call&& f) noexcept : _fun(std::forward<Call>(f)), _active(true) {}
    ScopeGuard(ScopeGuard&& rhs) noexcept : _fun(std::move(rhs._fun)), _active(rhs._active) {
        rhs.Dismiss();
    }
    ~ScopeGuard() noexcept {
        if (_active) {
            _fun();
        }
    }
    ScopeGuard() = delete;
    ScopeGuard(const ScopeGuard&) = delete;
    ScopeGuard& operator=(const ScopeGuard&) = delete;

    void Dismiss() noexcept { _active = false; }

private:
    Call _fun;
    bool _active;
};

template <typename Call>
auto MakeScopeGuard(Call&& f) noexcept { return ScopeGuard<Call>{std::forward<Call>(f)}; }

class DefaultMemoryResource : public std::pmr::memory_resource {
public:
    ~DefaultMemoryResource() noexcept override = default;

private:
    void* do_allocate(size_t bytes, size_t align) override;
    void do_deallocate(void* ptr, size_t bytes, size_t align) override;
    bool do_is_equal(const memory_resource& that) const noexcept override;
};

}  // namespace radray
