#pragma once

#include <radray/d3d12/utility.h>

namespace radray::d3d12 {

class Device;
class CommandAllocator;

class CommandQueue {
public:
    CommandQueue(Device* device, D3D12_COMMAND_LIST_TYPE type) noexcept;

    void Execute(CommandAllocator* alloc);
    void WaitFrame(uint64 frameIndex);
    void Flush();

    Device* device;
    D3D12_COMMAND_LIST_TYPE type;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> fence;
    uint64 lastFrame;
    uint64 executedFrame;
};

}  // namespace radray::d3d12
