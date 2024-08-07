#include "d3d12_fence.h"

#include "d3d12_device.h"

namespace radray::rhi::d3d12 {

Fence::Fence(Device* device)
    : fenceValue(1) {
    RADRAY_DX_FTHROW(device->device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.GetAddressOf())));
    waitEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
}

}  // namespace radray::rhi::d3d12
