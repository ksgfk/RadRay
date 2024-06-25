#pragma once

#include "d3d12_helper.h"

namespace radray::rhi::d3d12 {

class D3D12Texture {
public:
    D3D12Texture(
        ComPtr<ID3D12Resource> resource,
        ComPtr<D3D12MA::Allocation> alloc,
        D3D12_RESOURCE_STATES initState);
    virtual ~D3D12Texture() noexcept = default;

public:
    ComPtr<ID3D12Resource> resource;
    ComPtr<D3D12MA::Allocation> alloc;
    D3D12_RESOURCE_STATES initState;
};

}  // namespace radray::rhi::d3d12
