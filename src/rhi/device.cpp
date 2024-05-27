#include <radray/rhi/device.h>

#include <radray/logger.h>

#if defined(RADRAY_ENABLE_D3D12)
#include "d3d12/device.h"
#endif

namespace radray::rhi {

std::shared_ptr<IDevice> CreateDeviceD3D12(const DeviceCreateInfoD3D12& info) {
#if defined(RADRAY_ENABLE_D3D12)
    return std::make_shared<d3d12::Device>(info);
#else
    RADRAY_ERR_LOG("d3d12 is unavailable");
    return nullptr;
#endif
}

}  // namespace radray::rhi
