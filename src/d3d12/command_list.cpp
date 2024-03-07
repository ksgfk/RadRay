#include <radray/d3d12/command_list.h>

#include <radray/d3d12/device.h>
#include <radray/d3d12/command_allocator.h>
#include <radray/d3d12/upload_buffer.h>

namespace radray::d3d12 {

CommandList::CommandList(Device* device, CommandAllocator* allocator) noexcept : device(device), alloc(allocator), isOpen(false) {
    ThrowIfFailed(device->device->CreateCommandList(
        0,
        allocator->type,
        allocator->alloc.Get(),
        nullptr,
        IID_PPV_ARGS(cmd.GetAddressOf())));
    ThrowIfFailed(cmd->Close());
}

void CommandList::Reset() {
    auto old = isOpen;
    isOpen = true;
    if (old) {
        return;
    }
    ThrowIfFailed(cmd->Reset(alloc->alloc.Get(), nullptr));
}

void CommandList::Close() {
    auto old = isOpen;
    isOpen = false;
    if (!old) {
        return;
    }
    ThrowIfFailed(cmd->Close());
}

void CommandList::Upload(ID3D12Resource* dst, uint64 dstOffset, std::span<const uint8> src) {
    auto&& view = alloc->uploadAlloc.Allocate(src.size());
    view.handle->CopyData(view.offset, src);
    cmd->CopyBufferRegion(dst, dstOffset, view.handle->GetResource(), view.offset, src.size());
}

}  // namespace radray::d3d12
