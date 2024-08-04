#include <radray/rhi/device.h>

#include <radray/logger.h>

namespace radray::rhi {

std::shared_ptr<Device> CreateDeviceD3D12(const RadrayDeviceDescriptorD3D12* desc) {
#ifdef RADRAY_ENABLE_D3D12
    return d3d12::CreateImpl(info);
#else
    RADRAY_ERR_LOG("cannot create D3D12 device. D3D12 disabled");
    return nullptr;
#endif
}

std::shared_ptr<Device> CreateDeviceMetal(const RadrayDeviceDescriptorMetal* desc) {
#ifdef RADRAY_ENABLE_METAL
    return metal::CreateImpl(info);
#else
    RADRAY_ERR_LOG("cannot create Metal device. Metal disabled");
    return nullptr;
#endif
}

}  // namespace radray::rhi
