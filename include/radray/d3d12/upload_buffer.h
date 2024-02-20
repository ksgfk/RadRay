#pragma once

#include <span>
#include <radray/d3d12/buffer.h>

namespace radray::d3d12 {

class UploadBuffer : public Buffer {
public:
    UploadBuffer(Device* device, uint64 byteSize, IGpuHeapAllocator* allocator = nullptr) noexcept;
    ~UploadBuffer() noexcept override;
    UploadBuffer(UploadBuffer&&) noexcept = default;
    UploadBuffer(const UploadBuffer&) noexcept = delete;
    UploadBuffer& operator=(UploadBuffer&&) noexcept = default;
    UploadBuffer& operator=(const UploadBuffer&) noexcept = delete;

    D3D12_GPU_VIRTUAL_ADDRESS GetAddress() const noexcept override;
    uint64 GetByteSize() const noexcept override;
    ID3D12Resource* GetResource() const noexcept override;
    D3D12_RESOURCE_STATES GetInitState() const noexcept override;
    void* GetMapper() const noexcept { return _mapper; }
    void CopyData(uint64 offset, std::span<uint8 const> data) const noexcept;

private:
    ComPtr<ID3D12Resource> _resource;
    std::shared_ptr<IGpuHeapAllocation> _alloc;
    uint64 _byteSize;
    void* _mapper;
};

}  // namespace radray::d3d12
