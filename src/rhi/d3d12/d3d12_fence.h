#pragma once

#include "d3d12_helper.h"

namespace radray::rhi::d3d12 {

class Device;

class Fence {
public:
    Fence(Device* device);
    ~Fence() noexcept;

public:
    ComPtr<ID3D12Fence> fence;
    uint64_t fenceValue;
    HANDLE waitEvent;
};

}  // namespace radray::rhi::d3d12
