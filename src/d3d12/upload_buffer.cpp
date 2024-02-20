#include <radray/d3d12/upload_buffer.h>

#include <cstring>
#include <radray/d3d12/device.h>

namespace radray::d3d12 {

UploadBuffer::UploadBuffer(Device* device, uint64 byteSize, IGpuHeapAllocator* allocator) noexcept
    : Buffer(device),
      _byteSize(byteSize) {
    if (allocator) {
        _alloc = allocator->AllocBufferHeap(byteSize, D3D12_HEAP_TYPE_UPLOAD);
        auto buffer = CD3DX12_RESOURCE_DESC::Buffer(byteSize);
        auto [heap, offset] = _alloc->GetHeap();
        ThrowIfFailed(device->device->CreatePlacedResource(
            heap, offset,
            &buffer,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(_resource.GetAddressOf())));
    } else {
        auto prop = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
        auto buffer = CD3DX12_RESOURCE_DESC::Buffer(byteSize);
        ThrowIfFailed(device->device->CreateCommittedResource(
            &prop,
            D3D12_HEAP_FLAG_NONE,
            &buffer,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(_resource.GetAddressOf())));
    }
    ThrowIfFailed(_resource->Map(0, nullptr, reinterpret_cast<void**>(&_mapper)));
}

UploadBuffer::~UploadBuffer() noexcept {
    if (_mapper) {
        _resource->Unmap(0, nullptr);
        _mapper = nullptr;
    }
}

D3D12_GPU_VIRTUAL_ADDRESS UploadBuffer::GetAddress() const noexcept {
    return _resource->GetGPUVirtualAddress();
}

uint64 UploadBuffer::GetByteSize() const noexcept {
    return _byteSize;
}

ID3D12Resource* UploadBuffer::GetResource() const noexcept {
    return _resource.Get();
}

D3D12_RESOURCE_STATES UploadBuffer::GetInitState() const noexcept {
    return D3D12_RESOURCE_STATE_GENERIC_READ;
}

void UploadBuffer::CopyData(uint64 offset, std::span<uint8 const> data) const noexcept {
    std::memcpy(reinterpret_cast<uint8*>(_mapper) + offset, data.data(), data.size());
}

}  // namespace radray::d3d12
