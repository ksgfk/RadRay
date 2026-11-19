#include <radray/gpu/external_resource.h>

#include <limits>
#include <utility>

#if defined(RADRAY_PLATFORM_WINDOWS)
#include <radray/platform/win32_headers.h>
#endif

namespace radray::gpu {

OwnedExternalHandle::OwnedExternalHandle(uintptr_t value) noexcept : _value(value) {}
OwnedExternalHandle::~OwnedExternalHandle() noexcept { Reset(); }
OwnedExternalHandle::OwnedExternalHandle(OwnedExternalHandle&& other) noexcept : _value(std::exchange(other._value, 0)) {}
OwnedExternalHandle& OwnedExternalHandle::operator=(OwnedExternalHandle&& other) noexcept {
    if (this != &other) {
        Reset();
        _value = std::exchange(other._value, 0);
    }
    return *this;
}
void OwnedExternalHandle::Reset() noexcept {
#if defined(RADRAY_PLATFORM_WINDOWS)
    if (IsValid()) CloseHandle(reinterpret_cast<HANDLE>(_value));
#endif
    _value = 0;
}
ExternalResult<OwnedExternalHandle> OwnedExternalHandle::Duplicate() const noexcept {
#if defined(RADRAY_PLATFORM_WINDOWS)
    HANDLE result = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), reinterpret_cast<HANDLE>(_value), GetCurrentProcess(), &result, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
        const auto error = GetLastError();
        return ExternalError{"DuplicateHandle failed for the external handle", ExternalNativeApi::Win32, error};
    }
    return OwnedExternalHandle{reinterpret_cast<uintptr_t>(result)};
#else
    return ExternalError{"External NT handle duplication requires Windows"};
#endif
}

BorrowedExternalHandle OwnedExternalHandle::Borrow() const noexcept {
    return {_value};
}

bool OwnedExternalHandle::IsValid() const noexcept {
    return _value != 0 && _value != std::numeric_limits<uintptr_t>::max();
}

}  // namespace radray::gpu
