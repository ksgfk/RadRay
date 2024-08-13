#pragma once

#include "d3d12_helper.h"

namespace radray::rhi::d3d12 {

class Device;

class Texture {
public:
    Texture(
        Device* device,
        D3D12_RESOURCE_STATES initState,
        const D3D12_CLEAR_VALUE* cvPtr,
        const D3D12_RESOURCE_DESC& resDesc,
        const D3D12MA::ALLOCATION_DESC& allocDesc);

public:
    D3D12_RESOURCE_DESC desc;
    D3D12_RESOURCE_STATES initState;
    ComPtr<ID3D12Resource> texture;
    ComPtr<D3D12MA::Allocation> alloc;
    D3D12_GPU_VIRTUAL_ADDRESS gpuAddr;
};

}  // namespace radray::rhi::d3d12
