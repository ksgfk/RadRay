#pragma once

#include <radray/rhi/ctypes.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void* RadrayDevice;

RadrayDevice RadrayCreateDeviceD3D12(const RadrayDeviceDescriptorD3D12* desc);

RadrayDevice RadrayCreateDeviceMetal(const RadrayDeviceDescriptorMetal* desc);

void RadrayReleaseDevice(RadrayDevice device);

#ifdef __cplusplus
}
#endif
