#include "d3d12_command_queue.h"

#include "d3d12_device.h"
#include "d3d12_fence.h"

namespace radray::rhi::d3d12 {

CommandQueue::CommandQueue(Device* device, D3D12_COMMAND_LIST_TYPE type) {
    D3D12_COMMAND_QUEUE_DESC desc{
        .Type = type};
    RADRAY_DX_CHECK(device->device->CreateCommandQueue(&desc, IID_PPV_ARGS(queue.GetAddressOf())));
    fence = radray::make_unique<Fence>(device);
}

}  // namespace radray::rhi::d3d12
