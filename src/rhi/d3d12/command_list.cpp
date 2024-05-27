#include "command_list.h"

#include "device.h"
#include "command_allocator.h"

namespace radray::rhi::d3d12 {

CommandList::CommandList(Device* device, CommandAllocator* allocator) noexcept
    : device(device),
      alloc(allocator),
      isOpen(false) {
    RADRAY_DX_CHECK(device->device->CreateCommandList(
        0,
        allocator->type,
        allocator->alloc.Get(),
        nullptr,
        IID_PPV_ARGS(cmd.GetAddressOf())));
    RADRAY_DX_CHECK(cmd->Close());
}

void CommandList::Reset() {
    auto old = isOpen;
    isOpen = true;
    if (old) {
        return;
    }
    RADRAY_DX_CHECK(cmd->Reset(alloc->alloc.Get(), nullptr));
}

void CommandList::Close() {
    auto old = isOpen;
    isOpen = false;
    if (!old) {
        return;
    }
    RADRAY_DX_CHECK(cmd->Close());
}

}  // namespace radray::rhi::d3d12
