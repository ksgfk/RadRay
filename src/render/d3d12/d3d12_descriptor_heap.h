#pragma once

#include "d3d12_helper.h"

#include <radray/allocator.h>

namespace radray::render::d3d12 {

class DescriptorHeap {
public:
    DescriptorHeap(
        ID3D12Device* device,
        D3D12_DESCRIPTOR_HEAP_DESC desc) noexcept;

    ID3D12DescriptorHeap* Get() const noexcept;

    D3D12_DESCRIPTOR_HEAP_TYPE GetHeapType() const noexcept;

    UINT GetLength() const noexcept;

    bool IsShaderVisible() const noexcept;

    D3D12_GPU_DESCRIPTOR_HANDLE HandleGpu(UINT index) const noexcept;

    D3D12_CPU_DESCRIPTOR_HANDLE HandleCpu(UINT index) const noexcept;

    void Create(ID3D12Resource* resource, const D3D12_UNORDERED_ACCESS_VIEW_DESC& desc, UINT index) noexcept;

    void Create(ID3D12Resource* resource, const D3D12_SHADER_RESOURCE_VIEW_DESC& desc, UINT index) noexcept;

    void Create(const D3D12_CONSTANT_BUFFER_VIEW_DESC& desc, UINT index) noexcept;

    void Create(ID3D12Resource* resource, const D3D12_RENDER_TARGET_VIEW_DESC& desc, UINT index) noexcept;

    void Create(ID3D12Resource* resource, const D3D12_DEPTH_STENCIL_VIEW_DESC& desc, UINT index) noexcept;

    void Create(const D3D12_SAMPLER_DESC& desc, UINT index) noexcept;

    void CopyTo(UINT start, UINT count, DescriptorHeap* dst, UINT dstStart) noexcept;

private:
    ID3D12Device* _device;
    ComPtr<ID3D12DescriptorHeap> _heap;
    D3D12_DESCRIPTOR_HEAP_DESC _desc;
    D3D12_CPU_DESCRIPTOR_HANDLE _cpuStart;
    D3D12_GPU_DESCRIPTOR_HANDLE _gpuStart;
    UINT _incrementSize;
};

struct DescriptorHeapView {
    DescriptorHeap* Heap;
    UINT Start;
    UINT Length;

    static constexpr DescriptorHeapView Invalid() noexcept { return {nullptr, 0, 0}; }

    D3D12_GPU_DESCRIPTOR_HANDLE HandleGpu() const noexcept;

    D3D12_CPU_DESCRIPTOR_HANDLE HandleCpu() const noexcept;

    constexpr bool IsValid() const noexcept { return Heap != nullptr; }
};

class CpuDescriptorAllocatorImpl final : public BlockAllocator<BuddyAllocator, DescriptorHeap, CpuDescriptorAllocatorImpl> {
public:
    CpuDescriptorAllocatorImpl(
        ID3D12Device* device,
        D3D12_DESCRIPTOR_HEAP_TYPE type,
        UINT basicSize) noexcept;

    ~CpuDescriptorAllocatorImpl() noexcept override = default;

    radray::unique_ptr<DescriptorHeap> CreateHeap(size_t size) noexcept;

    BuddyAllocator CreateSubAllocator(size_t size) noexcept;

private:
    ID3D12Device* _device;
    D3D12_DESCRIPTOR_HEAP_TYPE _type;
};

class CpuDescriptorAllocator {
public:
    CpuDescriptorAllocator(
        ID3D12Device* device,
        D3D12_DESCRIPTOR_HEAP_TYPE type,
        UINT basicSize) noexcept;

    std::optional<DescriptorHeapView> Allocate(UINT count) noexcept;

    void Destroy(DescriptorHeapView view) noexcept;

private:
    CpuDescriptorAllocatorImpl _impl;
};

static_assert(is_allocator<CpuDescriptorAllocator, DescriptorHeapView>, "CpuDescriptorAllocator is not an allocator");

class GpuDescriptorAllocator {
public:
    GpuDescriptorAllocator(
        ID3D12Device* device,
        D3D12_DESCRIPTOR_HEAP_TYPE type,
        UINT size) noexcept;

    std::optional<DescriptorHeapView> Allocate(UINT count) noexcept;

    void Destroy(DescriptorHeapView view) noexcept;

    ID3D12DescriptorHeap* GetNative() const noexcept { return _heap->Get(); }
    DescriptorHeap* GetHeap() const noexcept { return _heap.get(); }

private:
    ID3D12Device* _device;
    radray::unique_ptr<DescriptorHeap> _heap;
    FreeListAllocator _allocator;
};

static_assert(is_allocator<GpuDescriptorAllocator, DescriptorHeapView>, "GpuDescriptorAllocator is not an allocator");

}  // namespace radray::render::d3d12
