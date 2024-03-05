#include <radray/d3d12/descriptor_heap.h>

#include <radray/d3d12/device.h>

namespace radray::d3d12 {

DescriptorHeap::DescriptorHeap(Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type, uint32 numDescriptors, bool shaderVisible) noexcept
    : _device(device) {
    D3D12_DESCRIPTOR_HEAP_DESC desc{
        .Type = type,
        .NumDescriptors = numDescriptors,
        .Flags = shaderVisible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE,
        .NodeMask = 0};
    ThrowIfFailed(device->device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(_descHeap.GetAddressOf())));
    _desc = desc;
    _cpuHeapStart = _descHeap->GetCPUDescriptorHandleForHeapStart();
    if (shaderVisible) {
        _gpuHeapStart = _descHeap->GetGPUDescriptorHandleForHeapStart();
    } else {
        _gpuHeapStart = {0};
    }
    _handleIncrementSize = device->device->GetDescriptorHandleIncrementSize(desc.Type);
}

D3D12_GPU_DESCRIPTOR_HANDLE DescriptorHeap::HandleGPU(uint64 index) const noexcept {
    if (index >= _desc.NumDescriptors) {
        RADRAY_ABORT("index out of range {}", index);
    }
    return CD3DX12_GPU_DESCRIPTOR_HANDLE(_gpuHeapStart, (INT)index, _handleIncrementSize);
}

D3D12_CPU_DESCRIPTOR_HANDLE DescriptorHeap::HandleCPU(uint64 index) const noexcept {
    if (index >= _desc.NumDescriptors) {
        RADRAY_ABORT("index out of range {}", index);
    }
    return CD3DX12_CPU_DESCRIPTOR_HANDLE(_cpuHeapStart, (INT)index, _handleIncrementSize);
}

void DescriptorHeap::CreateUav(ID3D12Resource* resource, const D3D12_UNORDERED_ACCESS_VIEW_DESC& pDesc, uint64 index) const noexcept {
    _device->device->CreateUnorderedAccessView(resource, nullptr, &pDesc, HandleCPU(index));
}

void DescriptorHeap::CreateSrv(ID3D12Resource* resource, const D3D12_SHADER_RESOURCE_VIEW_DESC& pDesc, uint64 index) const noexcept {
    _device->device->CreateShaderResourceView(resource, &pDesc, HandleCPU(index));
}

void DescriptorHeap::CreateRtv(ID3D12Resource* resource, const D3D12_RENDER_TARGET_VIEW_DESC& pDesc, uint64 index) const noexcept {
    _device->device->CreateRenderTargetView(resource, &pDesc, HandleCPU(index));
}

void DescriptorHeap::CreateDsv(ID3D12Resource* resource, const D3D12_DEPTH_STENCIL_VIEW_DESC& pDesc, uint64 index) const noexcept {
    _device->device->CreateDepthStencilView(resource, &pDesc, HandleCPU(index));
}

void DescriptorHeap::CreateSampler(const D3D12_SAMPLER_DESC& desc, uint64 index) const noexcept {
    _device->device->CreateSampler(&desc, HandleCPU(index));
}

}  // namespace radray::d3d12
