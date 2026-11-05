#include <radray/render/external_resource.h>

#include <utility>
#include <radray/logger.h>
#if defined(RADRAY_PLATFORM_WINDOWS)
#include <radray/platform/win32_headers.h>
#endif

namespace radray::render {
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

namespace detail {
std::string_view ExternalHandleName(ExternalMemoryHandleType type) noexcept {
    switch (type) {
        case ExternalMemoryHandleType::D3D12Resource: return "D3D12_RESOURCE";
        case ExternalMemoryHandleType::OpaqueWin32: return "OPAQUE_WIN32";
        default: return "unknown";
    }
}
std::string_view ExternalHandleName(ExternalFenceHandleType type) noexcept {
    switch (type) {
        case ExternalFenceHandleType::D3D12Fence: return "D3D12_FENCE";
        case ExternalFenceHandleType::OpaqueWin32: return "OPAQUE_WIN32";
        default: return "unknown";
    }
}

ExternalError LogExternalError(std::string_view operation, RenderBackend backend, ExternalError error, std::string_view parameters) noexcept {
    RADRAY_ERR_LOG("External resource {}: backend={} parameters={} error={} nativeApi={} nativeCode={}",
                   operation, backend, parameters, error.Message, static_cast<uint32_t>(error.NativeApi), error.NativeCode);
    return error;
}
}  // namespace detail
}  // namespace radray::render
