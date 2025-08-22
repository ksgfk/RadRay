#pragma once

#include <cstddef>
#include <utility>
#include <string_view>

namespace radray {

[[nodiscard]] void* Malloc(size_t size) noexcept;

void Free(void* ptr) noexcept;

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

    void* GetSymbol(std::string_view name) const noexcept;

    template <class T>
    requires std::is_function_v<T>
    auto GetFunction(std::string_view name) const noexcept {
        return reinterpret_cast<std::add_pointer_t<T>>(GetSymbol(name));
    }

    constexpr bool IsValid() const noexcept { return _handle != nullptr; }

    friend constexpr void swap(DynamicLibrary& l, DynamicLibrary& r) noexcept {
        std::swap(l._handle, r._handle);
    }

private:
    void* _handle{nullptr};
};

}  // namespace radray
