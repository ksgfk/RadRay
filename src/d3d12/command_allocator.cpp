#include <radray/d3d12/command_allocator.h>

#include <radray/d3d12/device.h>
#include <radray/d3d12/command_list.h>
#include <radray/d3d12/command_queue.h>

namespace radray::d3d12 {

CommandAllocator::CommandAllocator(Device* device, D3D12_COMMAND_LIST_TYPE type) noexcept : device(device), type(type) {
    ThrowIfFailed(device->device->CreateCommandAllocator(type, IID_PPV_ARGS(alloc.GetAddressOf())));
    cmd = std::make_unique<CommandList>(device, this);
}

void CommandAllocator::Execute(CommandQueue* queue, ID3D12Fence* fence, uint64 fenceIndex) {
    ID3D12CommandList* cmdList = cmd->cmd.Get();
    ID3D12CommandQueue* cmdQueue = queue->queue.Get();
    cmdQueue->ExecuteCommandLists(1, &cmdList);
    ThrowIfFailed(cmdQueue->Signal(fence, fenceIndex));
}

void CommandAllocator::Reset() {
    ThrowIfFailed(alloc->Reset());
    cmd->Reset();
}

}  // namespace radray::d3d12
