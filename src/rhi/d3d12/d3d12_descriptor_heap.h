#pragma once

#include "d3d12_helper.h"

namespace radray::rhi::d3d12 {

class Device;

class DescriptorHeap {
public:
    DescriptorHeap(
        Device* device,
        D3D12_DESCRIPTOR_HEAP_TYPE type,
        uint32_t length,
        bool isShaderVisible);

public:
    ComPtr<ID3D12DescriptorHeap> descHeap;
    D3D12_DESCRIPTOR_HEAP_DESC desc;
    D3D12_CPU_DESCRIPTOR_HANDLE cpuStart;
    D3D12_GPU_DESCRIPTOR_HANDLE gpuStart;
    UINT IncrementSize;
    uint32_t length;
};

}  // namespace radray::rhi::d3d12
