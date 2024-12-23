#pragma once

#include "d3d12_helper.h"

namespace radray::render::d3d12 {

class DescriptorHeap {
public:
    DescriptorHeap(
        DeviceD3D12* device,
        D3D12_DESCRIPTOR_HEAP_TYPE type,
        uint32_t length,
        bool isShaderVisible) noexcept;

    UINT Allocate() noexcept;
    void Recycle(UINT value) noexcept;
    void Clear() noexcept;
    void Reset() noexcept;

    D3D12_GPU_DESCRIPTOR_HANDLE HandleGpu(UINT index) const noexcept;
    D3D12_CPU_DESCRIPTOR_HANDLE HandleCpu(UINT index) const noexcept;

    void Create(ID3D12Resource* resource, const D3D12_UNORDERED_ACCESS_VIEW_DESC& desc, UINT index) noexcept;
    void Create(ID3D12Resource* resource, const D3D12_SHADER_RESOURCE_VIEW_DESC& desc, UINT index) noexcept;
    void Create(const D3D12_CONSTANT_BUFFER_VIEW_DESC& desc, UINT index) noexcept;
    void Create(ID3D12Resource* resource, const D3D12_RENDER_TARGET_VIEW_DESC& desc, UINT index) noexcept;
    void Create(ID3D12Resource* resource, const D3D12_DEPTH_STENCIL_VIEW_DESC& desc, UINT index) noexcept;
    void Create(const D3D12_SAMPLER_DESC& desc, UINT index) noexcept;

private:
    void ExpandCapacity() noexcept;

    DeviceD3D12* _device;
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
