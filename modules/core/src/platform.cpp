#include <radray/platform.h>

#include <radray/errors.h>
#include <radray/logger.h>

namespace radray {

void* Malloc(size_t size) noexcept {
#ifdef RADRAY_ENABLE_MIMALLOC
    return mi_malloc(size);
#else
    return malloc(size);
#endif
}

void Free(void* ptr) noexcept {
#ifdef RADRAY_ENABLE_MIMALLOC
    mi_free(ptr);
#else
    free(ptr);
#endif
}

DynamicLibrary::DynamicLibrary(DynamicLibrary&& other) noexcept : _handle(other._handle) {
    other._handle = nullptr;
}

DynamicLibrary& DynamicLibrary::operator=(DynamicLibrary&& other) noexcept {
    DynamicLibrary temp{std::move(other)};
    swap(*this, temp);
    return *this;
}

bool DynamicLibrary::IsValid() const noexcept {
    return _handle != nullptr;
}

}  // namespace radray

#ifdef RADRAY_PLATFORM_WINDOWS

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WINDOWS
#define _WINDOWS
#endif
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <windows.h>

namespace radray {

static auto _Win32LastErrMessage() {
    void* buffer = nullptr;
    auto errCode = ::GetLastError();
    ::FormatMessage(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        errCode,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        (LPTSTR)&buffer,
        0, nullptr);
    auto msg = format("{} (code = 0x{:x}).", static_cast<char*>(buffer), errCode);
    ::LocalFree(buffer);
    return msg;
}

void* AlignedAlloc(size_t alignment, size_t size) noexcept {
#ifdef RADRAY_ENABLE_MIMALLOC
    return mi_aligned_alloc(alignment, size);
#else
    return _aligned_malloc(size, alignment);
#endif
}

void AlignedFree(void* ptr) noexcept {
#ifdef RADRAY_ENABLE_MIMALLOC
    mi_free(ptr);
#else
    _aligned_free(ptr);
#endif
}

static_assert(sizeof(HMODULE) == sizeof(void*), "size of HMODULE not equal ptr?");
static_assert(sizeof(FARPROC) == sizeof(void*), "size of FARPROC not equal ptr?");

DynamicLibrary::DynamicLibrary(std::string_view name_) noexcept {
    string name;
    if (name_.ends_with(".dll")) {
        name = string{name_};
    } else {
        name = string{name_} + ".dll";
    }
    HMODULE m = ::LoadLibraryA(name.c_str());
    if (m == nullptr) [[unlikely]] {
        RADRAY_ERR_LOG("{} {} {}", Errors::InvalidOperation, "name", _Win32LastErrMessage());
    }
    _handle = m;
}

DynamicLibrary::~DynamicLibrary() noexcept {
    Destroy();
}

void* DynamicLibrary::GetSymbol(std::string_view name_) const noexcept {
    string name{name_};
    auto symbol = ::GetProcAddress(std::bit_cast<HMODULE>(_handle), name.c_str());
    if (symbol == nullptr) [[unlikely]] {
        RADRAY_ERR_LOG("{} {} {}", Errors::InvalidOperation, _Win32LastErrMessage(), name_);
    }
    return std::bit_cast<void*>(symbol);
}

void DynamicLibrary::Destroy() noexcept {
    if (_handle != nullptr) {
        ::FreeLibrary(std::bit_cast<HMODULE>(_handle));
        _handle = nullptr;
    }
}

}  // namespace radray

#else

#include <dlfcn.h>

#include <cstdlib>
#include <string>

namespace radray {

void* AlignedAlloc(size_t alignment, size_t size) noexcept {
#ifdef RADRAY_ENABLE_MIMALLOC
    return mi_aligned_alloc(alignment, size);
#else
    return std::aligned_alloc(alignment, size);
#endif
}

void AlignedFree(void* ptr) noexcept {
#ifdef RADRAY_ENABLE_MIMALLOC
    mi_free(ptr);
#else
    std::free(ptr);
#endif
}

DynamicLibrary::DynamicLibrary(std::string_view name_) noexcept {
    string name;
#ifdef RADRAY_PLATFORM_MACOS
    if (name_.starts_with("lib") && name_.ends_with(".dylib")) {
        name = string{name_};
    } else {
        name = "lib" + string{name_} + ".dylib";
    }
#elif RADRAY_PLATFORM_LINUX
    if (name_.starts_with("lib") && name_.ends_with(".so")) {
        name = string{name_};
    } else {
        name = "lib" + string{name_} + ".so";
    }
#else
#error "unknown platform"
#endif
    auto h = dlopen(name.c_str(), RTLD_LAZY);
    if (h == nullptr) {
        RADRAY_ERR_LOG("{} {} {}", Errors::InvalidOperation, "name", dlerror());
    }
    _handle = h;
}

DynamicLibrary::~DynamicLibrary() noexcept {
    Destroy();
}

void* DynamicLibrary::GetSymbol(std::string_view name_) const noexcept {
    string name{name_};
    auto symbol = dlsym(_handle, name.c_str());
    if (symbol == nullptr) [[unlikely]] {
        RADRAY_ERR_LOG("{} {} {}", Errors::InvalidOperation, "name", dlerror());
    }
    return symbol;
}

void DynamicLibrary::Destroy() noexcept {
    if (_handle != nullptr) {
        dlclose(_handle);
        _handle = nullptr;
    }
}

}  // namespace radray

#endif

namespace radray {

string FormatLastErrorMessageWin32() noexcept {
#ifdef RADRAY_PLATFORM_WINDOWS
    return _Win32LastErrMessage();
#else
    RADRAY_ERR_LOG("{}", Errors::UnsupportedPlatform);
    return "";
#endif
}

}  // namespace radray
