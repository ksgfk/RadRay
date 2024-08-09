#pragma once

#include "d3d12_helper.h"

namespace radray::rhi::d3d12 {

class Device;

class Buffer {
public:
    Buffer(
        Device* device,
        uint64_t size,
        D3D12_RESOURCE_STATES initState,
        const D3D12_RESOURCE_DESC& resDesc,
        const D3D12MA::ALLOCATION_DESC& allocDesc);

public:
    ComPtr<ID3D12Resource> buffer;
    ComPtr<D3D12MA::Allocation> alloc;
    uint64_t size;
    D3D12_RESOURCE_STATES initState;
};

}  // namespace radray::rhi::d3d12
