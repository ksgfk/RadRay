#include "buffer.h"

#include "device.h"

namespace radray::rhi::d3d12 {

Buffer::Buffer(
    std::shared_ptr<Device>&& device,
    BufferType type,
    uint64_t byteSize) noexcept
    : IBuffer(
          std::move(device),
          type,
          byteSize) {
}

D3D12_GPU_VIRTUAL_ADDRESS Buffer::GetAddress() const {
    return _resource->GetGPUVirtualAddress();
}

}  // namespace radray::rhi::d3d12
