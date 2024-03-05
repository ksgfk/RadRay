#include <radray/d3d12/command_queue.h>

#include <radray/d3d12/device.h>
#include <radray/d3d12/command_allocator.h>

namespace radray::d3d12 {

CommandQueue::CommandQueue(Device* device, D3D12_COMMAND_LIST_TYPE type) noexcept : device(device), type(type), lastFrame(0), executedFrame(0) {
    D3D12_COMMAND_QUEUE_DESC desc{
        .Type = type};
    ThrowIfFailed(device->device->CreateCommandQueue(&desc, IID_PPV_ARGS(queue.GetAddressOf())));
    ThrowIfFailed(device->device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.GetAddressOf())));
}

void CommandQueue::Execute(CommandAllocator* alloc) {
    auto currFrame = ++lastFrame;
    alloc->Execute(this, fence.Get(), currFrame);
}

void CommandQueue::WaitFrame(uint64 frameIndex) {
    if (frameIndex > lastFrame || frameIndex == 0) {
        return;
    }
    device->WaitFence(fence.Get(), frameIndex);
    executedFrame = frameIndex;
}

void CommandQueue::Flush() {
    auto currFrame = ++lastFrame;
    queue->Signal(fence.Get(), currFrame);
    device->WaitFence(fence.Get(), currFrame);
    executedFrame = currFrame;
}

}  // namespace radray::d3d12
