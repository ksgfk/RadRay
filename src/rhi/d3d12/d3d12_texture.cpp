#include "d3d12_texture.h"

namespace radray::rhi::d3d12 {

D3D12Texture::D3D12Texture(
    ComPtr<ID3D12Resource> resource,
    ComPtr<D3D12MA::Allocation> alloc,
    D3D12_RESOURCE_STATES initState)
    : resource(std::move(resource)),
      alloc(std::move(alloc)),
      initState(initState) {}

}  // namespace radray::rhi::d3d12
