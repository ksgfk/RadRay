#include <radray/render/device.h>

#ifdef RADRAY_ENABLE_D3D12
#include "d3d12/d3d12_device.h"
#endif

namespace radray::render {

std::optional<radray::shared_ptr<Device>> CreateDevice(const DeviceDescriptor& desc) {
    return std::visit(
        [](auto&& arg) -> std::optional<radray::shared_ptr<Device>> {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, D3D12DeviceDescriptor>) {
#ifdef RADRAY_ENABLE_D3D12
                return d3d12::CreateDevice(arg);
#else
                RADRAY_ERR_LOG("D3D12 is not enable");
                return std::nullopt;
#endif
            } else if constexpr (std::is_same_v<T, MetalDeviceDescriptor>) {
                RADRAY_ERR_LOG("Metal is planing");
                return std::nullopt;
            } else if constexpr (std::is_same_v<T, VulkanDeviceDescriptor>) {
                RADRAY_ERR_LOG("Vulkan is planing");
                return std::nullopt;
            } else {
                static_assert(false, "unreachable");
            }
        },
        desc);
}

}  // namespace radray::render