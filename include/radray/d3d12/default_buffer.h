#pragma once

#include <radray/d3d12/buffer.h>

namespace radray::d3d12 {

class DefaultBuffer : public Buffer {
public:
    DefaultBuffer(
        Device* device,
        uint64 byteSize,
        IGpuHeapAllocator* allocator = nullptr,
        D3D12_RESOURCE_STATES initState = D3D12_RESOURCE_STATE_COMMON) noexcept;
    ~DefaultBuffer() noexcept override = default;
    DefaultBuffer(DefaultBuffer&&) noexcept = default;
    DefaultBuffer(const DefaultBuffer&) noexcept = delete;
    DefaultBuffer& operator=(DefaultBuffer&&) noexcept = default;
    DefaultBuffer& operator=(const DefaultBuffer&) noexcept = delete;

    D3D12_GPU_VIRTUAL_ADDRESS GetAddress() const noexcept override;
    uint64 GetByteSize() const noexcept override;
    ID3D12Resource* GetResource() const noexcept override;
    D3D12_RESOURCE_STATES GetInitState() const noexcept override;
    void SetInitState(D3D12_RESOURCE_STATES state) noexcept { _initState = state; }

private:
    ComPtr<ID3D12Resource> _resource;
    std::shared_ptr<IGpuHeapAllocation> _alloc;
    uint64 _byteSize;
    D3D12_RESOURCE_STATES _initState;
};

}  // namespace radray::d3d12
