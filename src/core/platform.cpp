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

static auto _Win32LastErrMessage() {
    void* buffer = nullptr;
    auto errCode = GetLastError();
    FormatMessage(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        errCode,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        (LPTSTR)&buffer,
        0, nullptr);
    auto msg = fmt::format("{} (code = 0x{:x}).", static_cast<char*>(buffer), errCode);
    LocalFree(buffer);
    return msg;
}

void* AlignedAlloc(size_t alignment, size_t size) noexcept {
    return _aligned_malloc(size, alignment);
}

void AlignedFree(void* ptr) noexcept {
    _aligned_free(ptr);
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
    HMODULE m = LoadLibraryA(name.c_str());
    if (m == nullptr) [[unlikely]] {
        RADRAY_ERR_LOG("cannot load dynamic library {}, reason: {}", name, _Win32LastErrMessage());
    }
    _handle = m;
}

DynamicLibrary::~DynamicLibrary() noexcept {
    if (_handle != nullptr) {
        FreeLibrary(reinterpret_cast<HMODULE>(_handle));
        _handle = nullptr;
    }
}

void* DynamicLibrary::GetSymbol(std::string_view name_) const noexcept {
    string name{name_};
    auto symbol = GetProcAddress(reinterpret_cast<HMODULE>(_handle), name.c_str());
    if (symbol == nullptr) [[unlikely]] {
        RADRAY_ERR_LOG("cannot find symbol {}, reason: {}", name, _Win32LastErrMessage());
    }
    return reinterpret_cast<void*>(symbol);
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

void* DynamicLibrary::GetSymbol(std::string_view name_) const noexcept {
    string name{name_};
    auto symbol = dlsym(_handle, name.c_str());
    if (symbol == nullptr) [[unlikely]] {
        RADRAY_ERR_LOG("cannot load symbol {}, reason: {}", name, dlerror());
    }
    return symbol;
}

}  // namespace radray

#endif
