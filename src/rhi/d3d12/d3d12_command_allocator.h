#pragma once

#include "d3d12_helper.h"

namespace radray::rhi::d3d12 {

class Device;

class CommandAllocator {
public:
    CommandAllocator(Device* device, D3D12_COMMAND_LIST_TYPE type);

public:
    ComPtr<ID3D12CommandAllocator> alloc;
    D3D12_COMMAND_LIST_TYPE type;
};

}  // namespace radray::rhi::d3d12
