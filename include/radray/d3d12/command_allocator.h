#pragma once

#include <radray/d3d12/utility.h>

namespace radray::d3d12 {

class Device;
class CommandList;
class CommandQueue;

class CommandAllocator {
public:
    CommandAllocator(Device* device, D3D12_COMMAND_LIST_TYPE type) noexcept;

    void Execute(CommandQueue* queue, ID3D12Fence* fence, uint64 fenceIndex);
    void Reset();

    Device* device;
    ComPtr<ID3D12CommandAllocator> alloc;
    D3D12_COMMAND_LIST_TYPE type;
    std::unique_ptr<CommandList> cmd;
};

}  // namespace radray::d3d12
