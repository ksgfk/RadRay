#include "d3d12_texture.h"

#include "d3d12_device.h"

namespace radray::rhi::d3d12 {

Texture::Texture(
    Device* device,
    const D3D12_RESOURCE_DESC& resDesc,
    D3D12_RESOURCE_STATES initState,
    const D3D12MA::ALLOCATION_DESC& allocDesc)
    : desc(resDesc),
      initState(initState) {
    RADRAY_DX_FTHROW(device->resourceAlloc->CreateResource(
        &allocDesc,
        &resDesc,
        initState,
        nullptr,
        alloc.GetAddressOf(),
        IID_PPV_ARGS(texture.GetAddressOf())));
    gpuAddr = texture->GetGPUVirtualAddress();
}

Texture::Texture(
    const ComPtr<ID3D12Resource>& res,
    D3D12_RESOURCE_STATES initState)
    : desc(res->GetDesc()),
      initState(initState),
      texture(res),
      alloc{},
      gpuAddr(texture->GetGPUVirtualAddress()) {}

}  // namespace radray::rhi::d3d12
