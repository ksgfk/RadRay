#include "d3d12_command_queue.h"

#include "d3d12_device.h"

namespace radray::rhi::d3d12 {

D3D12CommandQueue::D3D12CommandQueue(D3D12Device* device, D3D12_COMMAND_LIST_TYPE type) {
    D3D12_COMMAND_QUEUE_DESC desc{
        .Type = type};
    RADRAY_DX_CHECK(device->device->CreateCommandQueue(&desc, IID_PPV_ARGS(queue.GetAddressOf())));
}

}  // namespace radray::rhi::d3d12
