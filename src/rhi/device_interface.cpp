#include <radray/rhi/device_interface.h>

#include <radray/logger.h>
#ifdef RADRAY_ENABLE_D3D12
#include "d3d12/device.h"
#endif
#ifdef RADRAY_ENABLE_METAL
#include "metal/device.h"
#endif

namespace radray::rhi {

std::unique_ptr<DeviceInterface> CreateDeviceD3D12(const DeviceCreateInfoD3D12& info) {
#ifdef RADRAY_ENABLE_D3D12
    return d3d12::CreateImpl(info);
#else
    RADRAY_ERR_LOG("cannot create D3D12 device. D3D12 disabled");
    return nullptr;
#endif
}

std::unique_ptr<DeviceInterface> CreateDeviceMetal(const DeviceCreateInfoMetal& info) {
#ifdef RADRAY_ENABLE_METAL
    return metal::CreateImpl(info);
#else
    RADRAY_ERR_LOG("cannot create Metal device. Metal disabled");
    return nullptr;
#endif
}

}  // namespace radray::rhi
