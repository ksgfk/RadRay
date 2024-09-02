#include "d3d12_descriptor_heap.h"

#include "d3d12_device.h"

namespace radray::rhi::d3d12 {

DescriptorHeap::DescriptorHeap(
    Device* device,
    D3D12_DESCRIPTOR_HEAP_TYPE type,
    uint32_t length,
    bool isShaderVisible)
    : device(device),
      desc{D3D12_DESCRIPTOR_HEAP_DESC{
          .Type = type,
          .NumDescriptors = length,
          .Flags = (isShaderVisible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE),
          .NodeMask = 0}},
      allocIndex{0} {
    RADRAY_DX_FTHROW(device->device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(heap.GetAddressOf())));
    cpuStart = heap->GetCPUDescriptorHandleForHeapStart();
    if (isShaderVisible) {
        gpuStart = heap->GetGPUDescriptorHandleForHeapStart();
    } else {
        gpuStart = {0};
    }
    incrementSize = device->device->GetDescriptorHandleIncrementSize(desc.Type);
    RADRAY_DEBUG_LOG("D3D12 DescriptorHeap type={} incrementSize={} length={} all={}(bytes)", (int)desc.Type, incrementSize, length, UINT64(length) * incrementSize);
}

UINT DescriptorHeap::Allocate() {
    UINT result;
    if (empty.empty()) {
        if (allocIndex == desc.NumDescriptors) {
            RADRAY_DX_THROW("DescriptorHeap is out of space {}", desc.NumDescriptors);
        }
        result = allocIndex;
        allocIndex++;
    } else {
        result = empty.back();
        empty.pop_back();
    }
    return result;
}

void DescriptorHeap::Recycle(UINT value) {
    empty.emplace_back(value);
}

void DescriptorHeap::Clear() {
    empty.clear();
    allocIndex = 0;
}

D3D12_GPU_DESCRIPTOR_HANDLE DescriptorHeap::HandleGpu(UINT index) const {
    return {gpuStart.ptr + UINT64(index) * UINT64(incrementSize)};
}

D3D12_CPU_DESCRIPTOR_HANDLE DescriptorHeap::HandleCpu(UINT index) const {
    return {cpuStart.ptr + UINT64(index) * UINT64(incrementSize)};
}

void DescriptorHeap::Create(ID3D12Resource* resource, const D3D12_UNORDERED_ACCESS_VIEW_DESC& desc, UINT index) {
    device->device->CreateUnorderedAccessView(resource, nullptr, &desc, HandleCpu(index));
}

void DescriptorHeap::Create(ID3D12Resource* resource, const D3D12_SHADER_RESOURCE_VIEW_DESC& desc, UINT index) {
    device->device->CreateShaderResourceView(resource, &desc, HandleCpu(index));
}

void DescriptorHeap::Create(const D3D12_CONSTANT_BUFFER_VIEW_DESC& desc, UINT index) {
    device->device->CreateConstantBufferView(&desc, HandleCpu(index));
}

void DescriptorHeap::Create(ID3D12Resource* resource, const D3D12_RENDER_TARGET_VIEW_DESC& desc, UINT index) {
    device->device->CreateRenderTargetView(resource, &desc, HandleCpu(index));
}

void DescriptorHeap::Create(ID3D12Resource* resource, const D3D12_DEPTH_STENCIL_VIEW_DESC& desc, UINT index) {
    device->device->CreateDepthStencilView(resource, &desc, HandleCpu(index));
}

void DescriptorHeap::Create(const D3D12_SAMPLER_DESC& desc, UINT index) {
    device->device->CreateSampler(&desc, HandleCpu(index));
}

}  // namespace radray::rhi::d3d12
