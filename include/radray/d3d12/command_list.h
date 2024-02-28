#pragma once

#include <radray/d3d12/utility.h>

namespace radray::d3d12 {

class Device;
class CommandAllocator;

class CommandList {
public:
    CommandList(Device* device, CommandAllocator* allocator) noexcept;

    void Reset();
    void Close();

    Device* device;
    ComPtr<ID3D12GraphicsCommandList> cmd;
    CommandAllocator* alloc;
    bool isOpen;
};

}  // namespace radray::d3d12
