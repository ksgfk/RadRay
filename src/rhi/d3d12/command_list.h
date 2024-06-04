#pragma once

#include "helper.h"

namespace radray::rhi::d3d12 {

class Device;
class CommandAllocator;

class CommandList {
public:
    CommandList(Device* device, CommandAllocator* allocator);

    void Reset();
    void Close();

    Device* device;
    ComPtr<ID3D12GraphicsCommandList> cmd;
    CommandAllocator* alloc;
    bool isOpen;
};

}  // namespace radray::rhi::d3d12
