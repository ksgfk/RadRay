#pragma once

#include "d3d12_helper.h"

namespace radray::rhi::d3d12 {

class Device;

class RootSignature {
public:
    RootSignature(Device* device, const D3D12_VERSIONED_ROOT_SIGNATURE_DESC& desc);

public:
    ComPtr<ID3D12RootSignature> rootSig;
};

}  // namespace radray::rhi::d3d12
