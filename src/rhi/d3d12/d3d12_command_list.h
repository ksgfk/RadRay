#pragma once

#include "d3d12_helper.h"

namespace radray::rhi::d3d12 {

class Device;

class CommandList {
public:
    CommandList(
        Device* device,
        ID3D12CommandAllocator* allocator,
        D3D12_COMMAND_LIST_TYPE type);

    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12CommandAllocator> alloc;
    bool isOpen;
};

}  // namespace radray::rhi::d3d12
