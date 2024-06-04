#pragma once

#include <radray/rhi/command_queue.h>

#include "helper.h"

namespace radray::rhi::d3d12 {

class Device;
class CommandAllocator;

class CommandQueue : public ICommandQueue {
public:
    CommandQueue(std::shared_ptr<Device> device, D3D12_COMMAND_LIST_TYPE type);

    void Sync() override;

    void Execute(radray::rhi::CommandList&& cmd) override;

    void Execute(CommandAllocator* alloc);
    void WaitFrame(uint64_t frameIndex);
    void Flush();

    D3D12_COMMAND_LIST_TYPE type;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> fence;
    uint64_t lastFrame;
    uint64_t executedFrame;

private:
    Device* _Device() const noexcept;
};

}  // namespace radray::rhi::d3d12
