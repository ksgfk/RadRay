#pragma once

#include "d3d12_helper.h"

namespace radray::rhi::d3d12 {

class Device;

class CommandQueue {
public:
    CommandQueue(Device* device, D3D12_COMMAND_LIST_TYPE type);

public:
    ComPtr<ID3D12CommandQueue> queue;
};

}  // namespace radray::rhi::d3d12
