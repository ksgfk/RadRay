#include "buffer.h"

#include <radray/basic_math.h>
#include "device.h"

namespace radray::rhi::d3d12 {

Buffer::Buffer(
    std::shared_ptr<Device> device,
    D3D12MA::Allocator* allocator,
    BufferType type,
    uint64_t byteSize,
    D3D12_RESOURCE_STATES initState) noexcept
    : IBuffer(
          std::move(device),
          type,
          byteSize) {
    auto d3d12Type = ToHeapType(type);
    if (allocator) {
        D3D12MA::ALLOCATION_DESC desc{
            .Flags = D3D12MA::ALLOCATION_FLAGS::ALLOCATION_FLAG_STRATEGY_BEST_FIT,
            .HeapType = d3d12Type,
            .ExtraHeapFlags = D3D12_HEAP_FLAG_NONE,
            .CustomPool = nullptr,
            .pPrivateData = nullptr};
        D3D12_RESOURCE_ALLOCATION_INFO info{
            .SizeInBytes = CalcAlign(byteSize, D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT),
            .Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT};
        RADRAY_DX_CHECK(allocator->AllocateMemory(&desc, &info, _alloc.GetAddressOf()));
        auto heap = _alloc->GetHeap();
        auto offset = _alloc->GetOffset();
        auto buffer = CD3DX12_RESOURCE_DESC::Buffer(byteSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        RADRAY_DX_CHECK(device->device->CreatePlacedResource(
            heap, offset,
            &buffer,
            initState,
            nullptr,
            IID_PPV_ARGS(_resource.GetAddressOf())));
    } else {
        auto prop = CD3DX12_HEAP_PROPERTIES(d3d12Type);
        auto buffer = CD3DX12_RESOURCE_DESC::Buffer(byteSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        RADRAY_DX_CHECK(device->device->CreateCommittedResource(
            &prop,
            D3D12_HEAP_FLAG_NONE,
            &buffer,
            initState,
            nullptr,
            IID_PPV_ARGS(_resource.GetAddressOf())));
    }
}

D3D12_GPU_VIRTUAL_ADDRESS Buffer::GetAddress() const {
    return _resource->GetGPUVirtualAddress();
}

}  // namespace radray::rhi::d3d12
