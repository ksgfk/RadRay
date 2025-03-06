#pragma once

#include "d3d12_helper.h"

namespace radray::render::d3d12 {

// TODO: rewrite alloc
class DescriptorHeap {
public:
    DescriptorHeap(
        ID3D12Device* device,
        D3D12_DESCRIPTOR_HEAP_TYPE type,
        uint32_t length,
        bool isShaderVisible) noexcept;

    UINT Allocate() noexcept;
    void Recycle(UINT value) noexcept;
    void Clear() noexcept;
    void Reset() noexcept;
    UINT AllocateRange(UINT count) noexcept;
    void RecycleRange(UINT start, UINT count) noexcept;

    D3D12_GPU_DESCRIPTOR_HANDLE HandleGpu(UINT index) const noexcept;
    D3D12_CPU_DESCRIPTOR_HANDLE HandleCpu(UINT index) const noexcept;

    void Create(ID3D12Resource* resource, const D3D12_UNORDERED_ACCESS_VIEW_DESC& desc, UINT index) noexcept;
    void Create(ID3D12Resource* resource, const D3D12_SHADER_RESOURCE_VIEW_DESC& desc, UINT index) noexcept;
    void Create(const D3D12_CONSTANT_BUFFER_VIEW_DESC& desc, UINT index) noexcept;
    void Create(ID3D12Resource* resource, const D3D12_RENDER_TARGET_VIEW_DESC& desc, UINT index) noexcept;
    void Create(ID3D12Resource* resource, const D3D12_DEPTH_STENCIL_VIEW_DESC& desc, UINT index) noexcept;
    void Create(const D3D12_SAMPLER_DESC& desc, UINT index) noexcept;

    void CopyTo(UINT start, UINT count, DescriptorHeap* dst, UINT dstStart) noexcept;

    ID3D12DescriptorHeap* Get() const noexcept { return _heap.Get(); }

    D3D12_DESCRIPTOR_HEAP_TYPE GetHeapType() const noexcept { return _desc.Type; }

private:
    void ExpandCapacity(UINT need) noexcept;

    ID3D12Device* _device;
    D3D12_DESCRIPTOR_HEAP_DESC _desc;
    radray::vector<UINT> _empty;
    ComPtr<ID3D12DescriptorHeap> _heap;
    D3D12_CPU_DESCRIPTOR_HANDLE _cpuStart;
    D3D12_GPU_DESCRIPTOR_HANDLE _gpuStart;
    UINT _incrementSize;
    UINT _allocIndex;
    uint32_t _initLength;
};

}  // namespace radray::render::d3d12
