#include "external_resource.h"

#include <radray/logger.h>

namespace radray::gpu::detail {
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

ExternalError LogExternalError(std::string_view operation, GpuBackend backend, ExternalError error, std::string_view parameters) noexcept {
    RADRAY_ERR_LOG("External resource {}: backend={} parameters={} error={} nativeApi={} nativeCode={}",
                   operation, backend, parameters, error.Message, static_cast<uint32_t>(error.NativeApi), error.NativeCode);
    return error;
}
}  // namespace radray::gpu::detail
