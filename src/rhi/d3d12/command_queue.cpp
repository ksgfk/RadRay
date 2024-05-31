#include "command_queue.h"

#include "device.h"
#include "command_allocator.h"

namespace radray::rhi::d3d12 {

CommandQueue::CommandQueue(std::shared_ptr<Device> device_, D3D12_COMMAND_LIST_TYPE type) noexcept
    : ICommandQueue(std::move(device_)),
      type(type),
      lastFrame(0),
      executedFrame(0) {
    D3D12_COMMAND_QUEUE_DESC desc{
        .Type = type};
    RADRAY_DX_CHECK(_Device()->device->CreateCommandQueue(&desc, IID_PPV_ARGS(queue.GetAddressOf())));
    RADRAY_DX_CHECK(_Device()->device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.GetAddressOf())));
}

void CommandQueue::Sync() {
    Flush();
}

void CommandQueue::Execute(radray::rhi::CommandList&& cmd) {
    // TODO:
}

void CommandQueue::Execute(CommandAllocator* alloc) {
    auto currFrame = ++lastFrame;
    alloc->Execute(this, fence.Get(), currFrame);
}

void CommandQueue::WaitFrame(uint64_t frameIndex) {
    if (frameIndex > lastFrame || frameIndex == 0) {
        return;
    }
    _Device()->WaitFence(fence.Get(), frameIndex);
    executedFrame = frameIndex;
}

void CommandQueue::Flush() {
    auto currFrame = ++lastFrame;
    queue->Signal(fence.Get(), currFrame);
    _Device()->WaitFence(fence.Get(), currFrame);
    executedFrame = currFrame;
}

Device* CommandQueue::_Device() const noexcept {
    return static_cast<Device*>(GetDevice());
}

}  // namespace radray::rhi::d3d12
