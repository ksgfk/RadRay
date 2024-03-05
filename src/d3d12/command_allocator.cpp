#include <radray/d3d12/command_allocator.h>

#include <limits>

#include <radray/d3d12/device.h>
#include <radray/d3d12/command_list.h>
#include <radray/d3d12/command_queue.h>

namespace radray::d3d12 {

CommandAllocator::CommandAllocator(Device* device, D3D12_COMMAND_LIST_TYPE type) noexcept
    : device(device),
      type(type),
      _rtvAllocator(device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV),
      _dsvAllocator(device, D3D12_DESCRIPTOR_HEAP_TYPE_DSV),
      rtvHeap(&_rtvAllocator, 64),
      dsvHeap(&_dsvAllocator, 64),
      lastExecuteFenceIndex(0) {
    ThrowIfFailed(device->device->CreateCommandAllocator(type, IID_PPV_ARGS(alloc.GetAddressOf())));
    cmd = std::make_unique<CommandList>(device, this);
}

void CommandAllocator::Execute(CommandQueue* queue, ID3D12Fence* fence, uint64 fenceIndex) {
    ID3D12CommandList* cmdList = cmd->cmd.Get();
    ID3D12CommandQueue* cmdQueue = queue->queue.Get();
    cmdQueue->ExecuteCommandLists(1, &cmdList);
    ThrowIfFailed(cmdQueue->Signal(fence, fenceIndex));
    lastExecuteFenceIndex = fenceIndex;
}

void CommandAllocator::Reset() {
    rtvHeap.Clear();
    dsvHeap.Clear();
    ThrowIfFailed(alloc->Reset());
    cmd->Reset();
}

CommandAllocator::CpuDescriptorHeapAllocator::CpuDescriptorHeapAllocator(
    Device* device,
    D3D12_DESCRIPTOR_HEAP_TYPE type) noexcept
    : device(device),
      type(type) {}

DescriptorHeap* CommandAllocator::CpuDescriptorHeapAllocator::Allocate(uint64 size) noexcept {
    if (size > std::numeric_limits<uint32>::max()) {
        RADRAY_ABORT("desc heap size out of range {}", size);
    }
    return new DescriptorHeap(device, type, static_cast<uint32>(size), false);
}

void CommandAllocator::CpuDescriptorHeapAllocator::Destroy(DescriptorHeap* handle) noexcept {
    delete handle;
}

}  // namespace radray::d3d12
