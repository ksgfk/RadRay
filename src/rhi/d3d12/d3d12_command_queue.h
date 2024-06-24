#pragma once

#include "d3d12_helper.h"

namespace radray::rhi::d3d12 {

class D3D12Device;

class D3D12CommandQueue {
public:
    D3D12CommandQueue(D3D12Device* device, D3D12_COMMAND_LIST_TYPE type);

public:
    ComPtr<ID3D12CommandQueue> queue;
};

}  // namespace radray::rhi::d3d12
