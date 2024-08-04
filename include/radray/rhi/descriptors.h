#pragma once

#ifndef __cplusplus
#include <stdbool.h>
#endif

#include <radray/rhi/defines.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct RadrayDeviceMemoryManagementDescriptor {
    void* (*Alloc)(size_t size, size_t align, void* userPtr);
    void (*Release)(void* ptr, void* userPtr);
    void* UserPtr;
} RadrayDeviceMemoryManagementDescriptor;

typedef struct RadrayDeviceDescriptorD3D12 {
    RadrayDeviceMemoryManagementDescriptor* Memory;
    uint32_t AdapterIndex;
    bool IsEnableDebugLayer;
} RadrayDeviceDescriptorD3D12;

typedef struct RadrayDeviceDescriptorMetal {
    RadrayDeviceMemoryManagementDescriptor* Memory;
    uint32_t DeviceIndex;
} RadrayDeviceDescriptorMetal;

#ifdef __cplusplus
}
#endif
