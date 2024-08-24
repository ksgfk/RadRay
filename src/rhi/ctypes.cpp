#include <radray/rhi/ctypes.h>

#include <radray/logger.h>
#include <radray/rhi/device_interface.h>
#ifdef RADRAY_ENABLE_D3D12
#include "d3d12/d3d12_device.h"
#endif

RadrayDevice RadrayCreateDeviceD3D12(const RadrayDeviceDescriptorD3D12* desc) {
    using namespace radray;
    using namespace radray::rhi;
#ifdef RADRAY_ENABLE_D3D12
    try {
        return new d3d12::Device(*desc);
    } catch (const std::exception& e) {
        RADRAY_ERR_LOG("{}", e.what());
        return nullptr;
    }
#else
    RADRAY_ERR_LOG("cannot create D3D12 device. D3D12 disabled");
    return nullptr;
#endif
}

RadrayDevice RadrayCreateDeviceMetal(const RadrayDeviceDescriptorMetal* desc) {
    RADRAY_ERR_LOG("cannot create Metal device. no Metal impl");
    return nullptr;
}

void RadrayReleaseDevice(RadrayDevice device) {
    delete reinterpret_cast<radray::rhi::DeviceInterface*>(device);
}
