#pragma once

#include "d3d12_helper.h"

namespace radray::rhi::d3d12 {

class Device;

class Buffer {
public:
    virtual ~Buffer() noexcept = default;

public:
    ComPtr<ID3D12Resource> buffer;
    ComPtr<D3D12MA::Allocation> alloc;
    uint64_t size;
    D3D12_RESOURCE_STATES initState;
};

class DefaultBuffer : public Buffer {
public:
    DefaultBuffer(
        Device* device,
        uint64_t size,
        D3D12_RESOURCE_STATES initState);
    ~DefaultBuffer() noexcept override = default;
};

}  // namespace radray::rhi::d3d12
