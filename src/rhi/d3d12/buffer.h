#pragma once

#include <radray/rhi/buffer.h>

#include "helper.h"

namespace radray::rhi::d3d12 {

class Device;

class Buffer : public IBuffer {
public:
    Buffer(
        std::shared_ptr<Device> device,
        D3D12MA::Allocator* allocator,
        BufferType type,
        uint64_t byteSize,
        D3D12_RESOURCE_STATES initState);
    ~Buffer() noexcept override = default;

    D3D12_GPU_VIRTUAL_ADDRESS GetAddress() const;
    ID3D12Resource* GetResource() const noexcept { return _resource.Get(); }
    D3D12_RESOURCE_STATES GetInitState() const noexcept { return _initState; }

private:
    ComPtr<ID3D12Resource> _resource;
    ComPtr<D3D12MA::Allocation> _alloc;
    D3D12_RESOURCE_STATES _initState;
};

}  // namespace radray::rhi::d3d12
