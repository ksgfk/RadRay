#include "d3d12_command_list.h"

#include "d3d12_device.h"

namespace radray::rhi::d3d12 {

CommandList::CommandList(
    Device* device,
    ID3D12CommandAllocator* allocator,
    D3D12_COMMAND_LIST_TYPE type)
    : alloc(allocator),
      isOpen(false) {
    RADRAY_DX_FTHROW(device->device->CreateCommandList(
        0,
        type,
        allocator,
        nullptr,
        IID_PPV_ARGS(list.GetAddressOf())));
    RADRAY_DX_FTHROW(list->Close());
}

}  // namespace radray::rhi::d3d12
