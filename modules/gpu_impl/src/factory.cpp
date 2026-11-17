#include <radray/gpu_impl/factory.h>

#include "rhi_validation.h"

#include <radray/logger.h>
#include <radray/utility.h>

#ifdef RADRAY_ENABLE_D3D12
#include <radray/gpu_impl/backend/d3d12_impl.h>
#include "d3d12/d3d12_diagnostics.h"
#endif

#ifdef RADRAY_ENABLE_VULKAN
#include <radray/gpu_impl/backend/vulkan_impl.h>
#endif

namespace radray::gpu {

Nullable<shared_ptr<Device>> CreateDevice(const DeviceDescriptor& desc, const RhiValidationOptions& validation) {
    auto device = std::visit(
        [](auto&& arg) -> Nullable<shared_ptr<Device>> {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, D3D12DeviceDescriptor>) {
#ifdef RADRAY_ENABLE_D3D12
                return d3d12::CreateDevice(arg);
#else
                RADRAY_ERR_LOG("D3D12 disable");
                return nullptr;
#endif
            } else if constexpr (std::is_same_v<T, VulkanDeviceDescriptor>) {
#ifdef RADRAY_ENABLE_VULKAN
                return vulkan::CreateDeviceVulkan(arg);
#else
                RADRAY_ERR_LOG("Vulkan disable");
                return nullptr;
#endif
            }
        },
        desc);
    if (!device || !validation.IsEnabled) return device;
    return detail::CreateValidationDevice(device.Release(), validation);
}

Nullable<InstanceVulkan*> InitVulkanEnv(const VulkanInstanceDescriptor& desc) {
#ifdef RADRAY_ENABLE_VULKAN
    return vulkan::InitVulkanEnvImpl(desc);
#else
    RADRAY_UNUSED(desc);
    RADRAY_ERR_LOG("Vulkan disable");
    return nullptr;
#endif
}

void ShutdownVulkanEnv() noexcept {
#ifdef RADRAY_ENABLE_VULKAN
    return vulkan::ShutdownVulkanEnvImpl();
#else
    RADRAY_ERR_LOG("Vulkan disable");
#endif
}

Nullable<shared_ptr<DXGIDiagnosticContext>> CreateDXGIDiagnosticContext() {
#ifdef RADRAY_ENABLE_D3D12
    return d3d12::detail::CreateDiagnosticContext();
#else
    RADRAY_ERR_LOG("D3D12 disable");
    return nullptr;
#endif
}

Nullable<unique_ptr<DXGIFactory>> CreateDXGIFactory(const DXGIFactoryDescriptor& desc) {
#ifdef RADRAY_ENABLE_D3D12
    return d3d12::CreateDXGIFactory(desc);
#else
    RADRAY_UNUSED(desc);
    RADRAY_ERR_LOG("D3D12 disable");
    return nullptr;
#endif
}

}  // namespace radray::gpu
