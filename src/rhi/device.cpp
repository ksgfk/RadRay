#include <radray/rhi/device.h>

#include <radray/logger.h>

#if defined(RADRAY_ENABLE_D3D12)
#include "d3d12/device.h"
#endif

namespace radray::rhi {

RC<IDevice> CreateDeviceD3D12(const DeviceCreateInfoD3D12& info) {
#if defined(RADRAY_ENABLE_D3D12)
    return MakeObject<d3d12::Device>(info);
#else
    RADRAY_ABORT("d3d12 is unavailable");
    return nullptr;
#endif
}

}  // namespace radray::rhi
