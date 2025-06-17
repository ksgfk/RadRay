#include <radray/render/device.h>

#include <array>

#include <radray/logger.h>

#ifdef RADRAY_ENABLE_D3D12
#include "d3d12/d3d12_device.h"
#endif

#ifdef RADRAY_ENABLE_METAL
#include "metal/metal_device.h"
#endif

#ifdef RADRAY_ENABLE_VULKAN
#include "vk/vulkan_device.h"
#endif

namespace radray::render {

Nullable<radray::shared_ptr<Device>> CreateDevice(const DeviceDescriptor& desc) {
    return std::visit(
        [](auto&& arg) -> Nullable<radray::shared_ptr<Device>> {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, D3D12DeviceDescriptor>) {
#ifdef RADRAY_ENABLE_D3D12
                return d3d12::CreateDevice(arg);
#else
                RADRAY_ERR_LOG("D3D12 is not enable");
                return nullptr;
#endif
            } else if constexpr (std::is_same_v<T, MetalDeviceDescriptor>) {
#ifdef RADRAY_ENABLE_METAL
                return metal::CreateDevice(arg);
#else
                RADRAY_ERR_LOG("Metal is not enable");
                return nullptr;
#endif
            } else if constexpr (std::is_same_v<T, VulkanDeviceDescriptor>) {
#ifdef RADRAY_ENABLE_VULKAN
                return vulkan::CreateDevice(arg);
#else
                RADRAY_ERR_LOG("Vulkan is not enable");
                return nullptr;
#endif
            } else {
                static_assert(false, "unreachable");
            }
        },
        desc);
}

static std::array<bool, static_cast<std::underlying_type_t<Backend>>(Backend::MAX_VALUE)> g_supportedBackends;

void GlobalInitGraphics(std::span<BackendInitDescriptor> desc) {
#ifdef RADRAY_ENABLE_VULKAN
    g_supportedBackends[static_cast<std::underlying_type_t<Backend>>(Backend::Vulkan)] = vulkan::GlobalInit(desc);
#endif
}

void GlobalTerminateGraphics() {
#ifdef RADRAY_ENABLE_VULKAN
    vulkan::GlobalTerminate();
    g_supportedBackends[static_cast<std::underlying_type_t<Backend>>(Backend::Vulkan)] = false;
#endif
}

}  // namespace radray::render
