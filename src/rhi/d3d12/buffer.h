#pragma once

#include <radray/rhi/buffer.h>

#include "helper.h"

namespace radray::rhi::d3d12 {

class Device;

class Buffer : public IBuffer {
public:
    Buffer(
        std::shared_ptr<Device>&& device,
        BufferType type,
        uint64_t byteSize) noexcept;
    ~Buffer() noexcept override = default;

    D3D12_GPU_VIRTUAL_ADDRESS GetAddress() const;

private:
    ComPtr<ID3D12Resource> _resource;
};

}  // namespace radray::rhi::d3d12
