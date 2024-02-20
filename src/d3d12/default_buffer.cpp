#include <radray/d3d12/default_buffer.h>

#include <radray/d3d12/device.h>

namespace radray::d3d12 {

DefaultBuffer::DefaultBuffer(
    Device* device,
    uint64 byteSize,
    IGpuHeapAllocator* allocator,
    D3D12_RESOURCE_STATES initState) noexcept
    : Buffer(device),
      _byteSize(byteSize),
      _initState(initState) {
    if (allocator) {
        _alloc = allocator->AllocBufferHeap(byteSize, D3D12_HEAP_TYPE_DEFAULT);
        auto buffer = CD3DX12_RESOURCE_DESC::Buffer(byteSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        auto [heap, offset] = _alloc->GetHeap();
        ThrowIfFailed(device->device->CreatePlacedResource(
            heap, offset,
            &buffer,
            initState,
            nullptr,
            IID_PPV_ARGS(_resource.GetAddressOf())));
    } else {
        auto prop = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        auto buffer = CD3DX12_RESOURCE_DESC::Buffer(byteSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        ThrowIfFailed(device->device->CreateCommittedResource(
            &prop,
            D3D12_HEAP_FLAG_NONE,
            &buffer,
            initState,
            nullptr,
            IID_PPV_ARGS(_resource.GetAddressOf())));
    }
}

D3D12_GPU_VIRTUAL_ADDRESS DefaultBuffer::GetAddress() const noexcept {
    return _resource->GetGPUVirtualAddress();
}

uint64 DefaultBuffer::GetByteSize() const noexcept {
    return _byteSize;
}

ID3D12Resource* DefaultBuffer::GetResource() const noexcept {
    return _resource.Get();
}

D3D12_RESOURCE_STATES DefaultBuffer::GetInitState() const noexcept {
    return _initState;
}

}  // namespace radray::d3d12
