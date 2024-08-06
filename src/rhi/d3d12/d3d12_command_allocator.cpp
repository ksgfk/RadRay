#include "d3d12_command_allocator.h"

#include "d3d12_device.h"

namespace radray::rhi::d3d12 {

CommandAllocator::CommandAllocator(Device* device, D3D12_COMMAND_LIST_TYPE type)
    : type(type) {
    RADRAY_DX_CHECK(device->device->CreateCommandAllocator(type, IID_PPV_ARGS(alloc.GetAddressOf())));
}

}  // namespace radray::rhi::d3d12
