#pragma once

#include "helper.h"
#include "command_list.h"

namespace radray::rhi::d3d12 {

class Device;
class CommandQueue;

class CommandAllocator {
public:
    CommandAllocator(Device* device, D3D12_COMMAND_LIST_TYPE type);

    void Execute(CommandQueue* queue, ID3D12Fence* fence, uint64_t fenceIndex);
    void Reset();

public:
    Device* device;
    ComPtr<ID3D12CommandAllocator> alloc;
    std::unique_ptr<CommandList> cmd;
    D3D12_COMMAND_LIST_TYPE type;
    uint64_t lastExecuteFenceIndex;
};

}  // namespace radray::rhi::d3d12
