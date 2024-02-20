#pragma once

#include <radray/d3d12/resource.h>

namespace radray::d3d12 {

class Buffer : public Resource {
public:
    Buffer(Device* device) noexcept;
    ~Buffer() noexcept override = default;
    Buffer(Buffer&&) noexcept = default;
    Buffer(const Buffer&) noexcept = delete;
    Buffer& operator=(Buffer&&) noexcept = default;
    Buffer& operator=(const Buffer&) noexcept = delete;

    virtual D3D12_GPU_VIRTUAL_ADDRESS GetAddress() const noexcept = 0;
    virtual uint64 GetByteSize() const noexcept = 0;
};

}  // namespace radray::d3d12
