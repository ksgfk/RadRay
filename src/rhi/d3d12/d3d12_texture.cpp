#include "d3d12_texture.h"

#include "d3d12_device.h"

namespace radray::rhi::d3d12 {

Texture::Texture(
    Device* device,
    D3D12_RESOURCE_STATES initState,
    const D3D12_CLEAR_VALUE* cvPtr,
    const D3D12_RESOURCE_DESC& resDesc,
    const D3D12MA::ALLOCATION_DESC& allocDesc)
    : desc(resDesc),
      initState(initState),
      clrValue(cvPtr ? *cvPtr : D3D12_CLEAR_VALUE{}) {
    RADRAY_DX_FTHROW(device->resourceAlloc->CreateResource(
        &allocDesc,
        &resDesc,
        initState,
        cvPtr,
        alloc.GetAddressOf(),
        IID_PPV_ARGS(texture.GetAddressOf())));
    gpuAddr = texture->GetGPUVirtualAddress();
}

}  // namespace radray::rhi::d3d12
