#include "command_allocator.h"

#include "device.h"
#include "command_queue.h"

namespace radray::rhi::d3d12 {

CommandAllocator::CommandAllocator(Device* device, D3D12_COMMAND_LIST_TYPE type)
    : device(device),
      type(type) {
    RADRAY_DX_CHECK(device->device->CreateCommandAllocator(type, IID_PPV_ARGS(alloc.GetAddressOf())));
    cmd = std::make_unique<CommandList>(device, this);
}

void CommandAllocator::Execute(CommandQueue* queue, ID3D12Fence* fence, uint64_t fenceIndex) {
    ID3D12CommandList* cmdList = cmd->cmd.Get();
    ID3D12CommandQueue* cmdQueue = queue->queue.Get();
    cmdQueue->ExecuteCommandLists(1, &cmdList);
    RADRAY_DX_CHECK(cmdQueue->Signal(fence, fenceIndex));
    lastExecuteFenceIndex = fenceIndex;
}

void CommandAllocator::Reset() {
    RADRAY_DX_CHECK(alloc->Reset());
    cmd->Reset();
}

}  // namespace radray::rhi::d3d12
