#pragma once

#include <cstddef>
#include <utility>
#include <string_view>
#include <bit>

#include <radray/types.h>

#ifndef _WINDEF_
struct HWND__;
using HWND = struct HWND__*;
#endif

namespace radray {

enum class PlatformId {
    UNKNOWN,
    Windows,
    Linux,
    MacOS
};

[[nodiscard]] void* Malloc(size_t size) noexcept;

void Free(void* ptr) noexcept;

[[nodiscard]] void* AlignedAlloc(size_t alignment, size_t size) noexcept;

void AlignedFree(void* ptr) noexcept;

PlatformId GetPlatform() noexcept;

class DynamicLibrary {
public:
    constexpr DynamicLibrary() noexcept = default;
    explicit DynamicLibrary(std::string_view name) noexcept;
    DynamicLibrary(const DynamicLibrary&) = delete;
    DynamicLibrary(DynamicLibrary&& other) noexcept;
    DynamicLibrary& operator=(const DynamicLibrary&) = delete;
    DynamicLibrary& operator=(DynamicLibrary&& other) noexcept;
    ~DynamicLibrary() noexcept;

    void* GetSymbol(std::string_view name) const noexcept;

    template <class T>
    requires std::is_pointer_v<T> && std::is_function_v<std::remove_pointer_t<T>>
    T GetFunction(std::string_view name) const noexcept {
        return std::bit_cast<T>(GetSymbol(name));
    }

    constexpr bool IsValid() const noexcept { return _handle != nullptr; }

    friend constexpr void swap(DynamicLibrary& l, DynamicLibrary& r) noexcept {
        std::swap(l._handle, r._handle);
    }

private:
    void* _handle{nullptr};
};

#if defined(RADRAY_PLATFORM_WINDOWS)
string FormatLastErrorMessageWin32() noexcept;
#endif

}  // namespace radray
