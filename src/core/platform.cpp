#include <radray/platform.h>

#include <radray/logger.h>

namespace radray {

DynamicLibrary::DynamicLibrary(DynamicLibrary&& other) noexcept : _handle(other._handle) {
    other._handle = nullptr;
}

DynamicLibrary& DynamicLibrary::operator=(DynamicLibrary&& other) noexcept {
    DynamicLibrary temp{std::move(other)};
    swap(*this, temp);
    return *this;
}

}  // namespace radray

#ifdef RADRAY_PLATFORM_WINDOWS

#include <windows.h>

namespace radray {

void* AlignedAlloc(size_t alignment, size_t size) noexcept {
    return _aligned_malloc(size, alignment);
}

void AlignedFree(void* ptr) noexcept {
    _aligned_free(ptr);
}

}  // namespace radray

#else

#include <dlfcn.h>

#include <cstdlib>
#include <string>

namespace radray {

void* AlignedAlloc(size_t alignment, size_t size) noexcept {
    return std::aligned_alloc(alignment, size);
}

void AlignedFree(void* ptr) noexcept {
    return std::free(ptr);
}

DynamicLibrary::DynamicLibrary(std::string_view name_) noexcept {
    std::string name;
#ifdef RADRAY_PLATFORM_MACOS
    if (name_.starts_with("lib") && name_.ends_with(".dylib")) {
        name = std::string{name_};
    } else {
        name = "lib" + std::string{name_} + ".dylib";
    }
#else
    if (name_.starts_with("lib") && name_.ends_with(".so")) {
        name = std::string{name_};
    } else {
        name = "lib" + std::string{name_} + ".so";
    }
#endif
    auto h = dlopen(name.c_str(), RTLD_LAZY);
    if (h == nullptr) {
        RADRAY_ERR_LOG("cannot load dynamic library {}, reason: {}", name, dlerror());
    }
    _handle = h;
}

DynamicLibrary::~DynamicLibrary() noexcept {
    if (_handle != nullptr) [[unlikely]] {
        dlclose(_handle);
        _handle = nullptr;
    }
}

void* DynamicLibrary::GetSymbol(std::string_view name_) noexcept {
    std::string name{name_};
    auto symbol = dlsym(_handle, name.c_str());
    if (symbol == nullptr) [[unlikely]] {
        RADRAY_ERR_LOG("cannot load symbol {}, reason: {}", name, dlerror());
    }
    return symbol;
}

}  // namespace radray

#endif
