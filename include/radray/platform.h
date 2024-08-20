#pragma once

#ifdef RADRAY_PLATFORM_WINDOWS
#define UNICODE
#define _UNICODE
#define NOMINMAX
#define _WINDOWS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <cstddef>
#include <utility>
#include <string_view>

namespace radray {

[[nodiscard]] void* AlignedAlloc(size_t alignment, size_t size) noexcept;

void AlignedFree(void* ptr) noexcept;

class DynamicLibrary {
public:
    constexpr DynamicLibrary() noexcept = default;
    explicit DynamicLibrary(std::string_view name) noexcept;
    DynamicLibrary(const DynamicLibrary&) = delete;
    DynamicLibrary(DynamicLibrary&& other) noexcept;
    DynamicLibrary& operator=(const DynamicLibrary&) = delete;
    DynamicLibrary& operator=(DynamicLibrary&& other) noexcept;
    ~DynamicLibrary() noexcept;

    void* GetSymbol(std::string_view name) noexcept;

    constexpr bool IsValid() const noexcept { return _handle != nullptr; }

    friend constexpr void swap(DynamicLibrary& l, DynamicLibrary& r) noexcept {
        std::swap(l._handle, r._handle);
    }

private:
    void* _handle{nullptr};
};

}  // namespace radray
