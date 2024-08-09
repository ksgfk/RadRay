#include "d3d12_buffer.h"

#include "d3d12_device.h"

namespace radray::rhi::d3d12 {

Buffer::Buffer(
    Device* device,
    uint64_t size,
    D3D12_RESOURCE_STATES initState,
    const D3D12_RESOURCE_DESC& resDesc,
    const D3D12MA::ALLOCATION_DESC& allocDesc)
    : size(size),
      initState(initState) {
    RADRAY_DX_FTHROW(device->resourceAlloc->CreateResource(&allocDesc, &resDesc, initState, nullptr, alloc.GetAddressOf(), IID_PPV_ARGS(buffer.GetAddressOf())));
}

}  // namespace radray::rhi::d3d12
