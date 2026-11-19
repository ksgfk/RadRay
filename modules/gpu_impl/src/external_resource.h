#pragma once

#include <string_view>

#include <radray/gpu/external_resource.h>

namespace radray::gpu::detail {
std::string_view ExternalHandleName(ExternalMemoryHandleType type) noexcept;
std::string_view ExternalHandleName(ExternalFenceHandleType type) noexcept;
ExternalError LogExternalError(std::string_view operation, GpuBackend backend, ExternalError error, std::string_view parameters) noexcept;
}  // namespace radray::gpu::detail
