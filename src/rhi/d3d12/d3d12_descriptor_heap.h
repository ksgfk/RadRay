#pragma once

#include "d3d12_helper.h"

namespace radray::rhi::d3d12 {

class Device;

class DescriptorHeap {
public:
    DescriptorHeap(
        Device* device,
        D3D12_DESCRIPTOR_HEAP_TYPE type,
        uint32_t length,
        bool isShaderVisible);

    UINT Allocate();
    void Recycle(UINT value);
    void Clear();

    D3D12_GPU_DESCRIPTOR_HANDLE HandleGpu(UINT index) const;
    D3D12_CPU_DESCRIPTOR_HANDLE HandleCpu(UINT index) const;

    void Create(ID3D12Resource* resource, const D3D12_UNORDERED_ACCESS_VIEW_DESC& desc, UINT index);
    void Create(ID3D12Resource* resource, const D3D12_SHADER_RESOURCE_VIEW_DESC& desc, UINT index);
    void Create(const D3D12_CONSTANT_BUFFER_VIEW_DESC& desc, UINT index);
    void Create(ID3D12Resource* resource, const D3D12_RENDER_TARGET_VIEW_DESC& desc, UINT index);
    void Create(ID3D12Resource* resource, const D3D12_DEPTH_STENCIL_VIEW_DESC& desc, UINT index);
    void Create(const D3D12_SAMPLER_DESC& desc, UINT index);

public:
    Device* device;
    D3D12_DESCRIPTOR_HEAP_DESC desc;
    radray::vector<UINT> empty;
    ComPtr<ID3D12DescriptorHeap> heap;
    D3D12_CPU_DESCRIPTOR_HANDLE cpuStart;
    D3D12_GPU_DESCRIPTOR_HANDLE gpuStart;
    UINT incrementSize;
    UINT allocIndex;
};

}  // namespace radray::rhi::d3d12
