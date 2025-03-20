#include "d3d12_descriptor_heap.h"

#include <cmath>

#include "d3d12_device.h"

namespace radray::render::d3d12 {

DescriptorHeap::DescriptorHeap(
    ID3D12Device* device,
    D3D12_DESCRIPTOR_HEAP_DESC desc) noexcept
    : _device(device) {
    RADRAY_DX_CHECK(_device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(_heap.GetAddressOf())));
    _desc = _heap->GetDesc();
    _cpuStart = _heap->GetCPUDescriptorHandleForHeapStart();
    _gpuStart = IsShaderVisible() ? _heap->GetGPUDescriptorHandleForHeapStart() : D3D12_GPU_DESCRIPTOR_HANDLE{0};
    _incrementSize = _device->GetDescriptorHandleIncrementSize(_desc.Type);
    RADRAY_DEBUG_LOG(
        "D3D12 create DescriptorHeap. Type={}, IsShaderVisible={}, IncrementSize={}, Length={}, all={}(bytes)",
        _desc.Type,
        IsShaderVisible(),
        _incrementSize,
        _desc.NumDescriptors,
        UINT64(_desc.NumDescriptors) * _incrementSize);
}

ID3D12DescriptorHeap* DescriptorHeap::Get() const noexcept {
    return _heap.Get();
}

D3D12_DESCRIPTOR_HEAP_TYPE DescriptorHeap::GetHeapType() const noexcept {
    return _desc.Type;
}

UINT DescriptorHeap::GetLength() const noexcept {
    return _desc.NumDescriptors;
}

bool DescriptorHeap::IsShaderVisible() const noexcept {
    return (_desc.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) == D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
}

D3D12_GPU_DESCRIPTOR_HANDLE DescriptorHeap::HandleGpu(UINT index) const noexcept {
    return {_gpuStart.ptr + UINT64(index) * UINT64(_incrementSize)};
}

D3D12_CPU_DESCRIPTOR_HANDLE DescriptorHeap::HandleCpu(UINT index) const noexcept {
    return {_cpuStart.ptr + UINT64(index) * UINT64(_incrementSize)};
}

void DescriptorHeap::Create(ID3D12Resource* resource, const D3D12_UNORDERED_ACCESS_VIEW_DESC& desc, UINT index) noexcept {
    _device->CreateUnorderedAccessView(resource, nullptr, &desc, HandleCpu(index));
}

void DescriptorHeap::Create(ID3D12Resource* resource, const D3D12_SHADER_RESOURCE_VIEW_DESC& desc, UINT index) noexcept {
    _device->CreateShaderResourceView(resource, &desc, HandleCpu(index));
}

void DescriptorHeap::Create(const D3D12_CONSTANT_BUFFER_VIEW_DESC& desc, UINT index) noexcept {
    _device->CreateConstantBufferView(&desc, HandleCpu(index));
}

void DescriptorHeap::Create(ID3D12Resource* resource, const D3D12_RENDER_TARGET_VIEW_DESC& desc, UINT index) noexcept {
    _device->CreateRenderTargetView(resource, &desc, HandleCpu(index));
}

void DescriptorHeap::Create(ID3D12Resource* resource, const D3D12_DEPTH_STENCIL_VIEW_DESC& desc, UINT index) noexcept {
    _device->CreateDepthStencilView(resource, &desc, HandleCpu(index));
}

void DescriptorHeap::Create(const D3D12_SAMPLER_DESC& desc, UINT index) noexcept {
    _device->CreateSampler(&desc, HandleCpu(index));
}

void DescriptorHeap::CopyTo(UINT start, UINT count, DescriptorHeap* dst, UINT dstStart) noexcept {
    _device->CopyDescriptorsSimple(
        count,
        dst->HandleCpu(dstStart),
        HandleCpu(start),
        _desc.Type);
}

D3D12_GPU_DESCRIPTOR_HANDLE DescriptorHeapView::HandleGpu() const noexcept {
    return Heap->HandleGpu(Start);
}

D3D12_CPU_DESCRIPTOR_HANDLE DescriptorHeapView::HandleCpu() const noexcept {
    return Heap->HandleCpu(Start);
}

CpuDescriptorAllocatorImpl::CpuDescriptorAllocatorImpl(
    ID3D12Device* device,
    D3D12_DESCRIPTOR_HEAP_TYPE type,
    UINT basicSize) noexcept
    : BlockAllocator(basicSize, 1),
      _device(device),
      _type(type) {}

radray::unique_ptr<DescriptorHeap> CpuDescriptorAllocatorImpl::CreateHeap(size_t size) noexcept {
    return radray::make_unique<DescriptorHeap>(
        _device,
        D3D12_DESCRIPTOR_HEAP_DESC{
            _type,
            static_cast<UINT>(size),
            D3D12_DESCRIPTOR_HEAP_FLAG_NONE,
            0});
}

BuddyAllocator CpuDescriptorAllocatorImpl::CreateSubAllocator(size_t size) noexcept { return BuddyAllocator{size}; }

CpuDescriptorAllocator::CpuDescriptorAllocator(
    ID3D12Device* device,
    D3D12_DESCRIPTOR_HEAP_TYPE type,
    UINT basicSize) noexcept
    : _impl(device, type, basicSize) {}

std::optional<DescriptorHeapView> CpuDescriptorAllocator::Allocate(UINT count) noexcept {
    auto allocation = _impl.Allocate(count);
    if (!allocation.has_value()) {
        return std::nullopt;
    }
    auto v = allocation.value();
    return std::make_optional(DescriptorHeapView{
        v.Heap,
        static_cast<UINT>(v.Start),
        static_cast<UINT>(v.Length)});
}

void CpuDescriptorAllocator::Destroy(DescriptorHeapView view) noexcept {
    _impl.Destroy({view.Heap, view.Start, view.Length});
}

GpuDescriptorAllocator::GpuDescriptorAllocator(
    ID3D12Device* device,
    D3D12_DESCRIPTOR_HEAP_TYPE type,
    UINT size) noexcept
    : _device(device),
      _heap(_device,
            D3D12_DESCRIPTOR_HEAP_DESC{
                type,
                static_cast<UINT>(size),
                D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,
                0}),
      _allocator(size) {}

std::optional<DescriptorHeapView> GpuDescriptorAllocator::Allocate(UINT count) noexcept {
    auto allocation = _allocator.Allocate(count);
    if (!allocation.has_value()) {
        return std::nullopt;
    }
    auto v = allocation.value();
    return std::make_optional(DescriptorHeapView{
        &_heap,
        static_cast<UINT>(v),
        count});
}

void GpuDescriptorAllocator::Destroy(DescriptorHeapView view) noexcept {
    _allocator.Destroy(view.Start);
}

}  // namespace radray::render::d3d12
