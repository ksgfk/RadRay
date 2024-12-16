#include "d3d12_descriptor_heap.h"

#include <cmath>

#include "d3d12_device.h"

namespace radray::render::d3d12 {

DescriptorHeap::DescriptorHeap(
    DeviceD3D12* device,
    D3D12_DESCRIPTOR_HEAP_TYPE type,
    uint32_t length,
    bool isShaderVisible) noexcept
    : _device(device),
      _desc{D3D12_DESCRIPTOR_HEAP_DESC{
          .Type = type,
          .NumDescriptors = length,
          .Flags = (isShaderVisible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE),
          .NodeMask = 0}},
      _allocIndex{0},
      _initLength(length) {
    RADRAY_DX_CHECK(device->_device->CreateDescriptorHeap(&_desc, IID_PPV_ARGS(_heap.GetAddressOf())));
    _cpuStart = _heap->GetCPUDescriptorHandleForHeapStart();
    if (isShaderVisible) {
        _gpuStart = _heap->GetGPUDescriptorHandleForHeapStart();
    } else {
        _gpuStart = {0};
    }
    _incrementSize = device->_device->GetDescriptorHandleIncrementSize(_desc.Type);
    RADRAY_DEBUG_LOG("D3D12 create DescHeap type={} isGpuVis={} incrementSize={} length={} all={}(bytes)",
                     _desc.Type,
                     isShaderVisible,
                     _incrementSize, length, UINT64(length) * _incrementSize);
}

UINT DescriptorHeap::Allocate() noexcept {
    UINT result;
    if (_empty.empty()) {
        if (_allocIndex == _desc.NumDescriptors) {
            ExpandCapacity();
        }
        result = _allocIndex;
        _allocIndex++;
    } else {
        result = _empty.back();
        _empty.pop_back();
    }
    return result;
}

void DescriptorHeap::Recycle(UINT value) noexcept {
    _empty.emplace_back(value);
}

void DescriptorHeap::Clear() noexcept {
    _empty.clear();
    _allocIndex = 0;
}

void DescriptorHeap::Reset() noexcept {
    Clear();
    _empty.shrink_to_fit();
    if (_desc.NumDescriptors != _initLength) {
        _desc.NumDescriptors = _initLength;
        _heap.Reset();
        RADRAY_DX_CHECK(_device->_device->CreateDescriptorHeap(&_desc, IID_PPV_ARGS(_heap.GetAddressOf())));
        _cpuStart = _heap->GetCPUDescriptorHandleForHeapStart();
        if (_desc.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) {
            _gpuStart = _heap->GetGPUDescriptorHandleForHeapStart();
        }
    }
}

D3D12_GPU_DESCRIPTOR_HANDLE DescriptorHeap::HandleGpu(UINT index) const noexcept {
    return {_gpuStart.ptr + UINT64(index) * UINT64(_incrementSize)};
}

D3D12_CPU_DESCRIPTOR_HANDLE DescriptorHeap::HandleCpu(UINT index) const noexcept {
    return {_cpuStart.ptr + UINT64(index) * UINT64(_incrementSize)};
}

void DescriptorHeap::Create(ID3D12Resource* resource, const D3D12_UNORDERED_ACCESS_VIEW_DESC& desc, UINT index) noexcept {
    _device->_device->CreateUnorderedAccessView(resource, nullptr, &desc, HandleCpu(index));
}

void DescriptorHeap::Create(ID3D12Resource* resource, const D3D12_SHADER_RESOURCE_VIEW_DESC& desc, UINT index) noexcept {
    _device->_device->CreateShaderResourceView(resource, &desc, HandleCpu(index));
}

void DescriptorHeap::Create(const D3D12_CONSTANT_BUFFER_VIEW_DESC& desc, UINT index) noexcept {
    _device->_device->CreateConstantBufferView(&desc, HandleCpu(index));
}

void DescriptorHeap::Create(ID3D12Resource* resource, const D3D12_RENDER_TARGET_VIEW_DESC& desc, UINT index) noexcept {
    _device->_device->CreateRenderTargetView(resource, &desc, HandleCpu(index));
}

void DescriptorHeap::Create(ID3D12Resource* resource, const D3D12_DEPTH_STENCIL_VIEW_DESC& desc, UINT index) noexcept {
    _device->_device->CreateDepthStencilView(resource, &desc, HandleCpu(index));
}

void DescriptorHeap::Create(const D3D12_SAMPLER_DESC& desc, UINT index) noexcept {
    _device->_device->CreateSampler(&desc, HandleCpu(index));
}

void DescriptorHeap::ExpandCapacity() noexcept {
    UINT now = _desc.NumDescriptors;
    UINT next = static_cast<UINT>(std::min(std::max(UINT64(now) + 1, static_cast<UINT64>(UINT64(now) * 1.5)), UINT64(std::numeric_limits<UINT>::max())));
    if (now == next) {
        RADRAY_ABORT("DescriptorHeap expand failed");
        return;
    }
    D3D12_DESCRIPTOR_HEAP_DESC nextDesc{
        .Type = _desc.Type,
        .NumDescriptors = next,
        .Flags = _desc.Flags,
        .NodeMask = _desc.NodeMask};
    ComPtr<ID3D12DescriptorHeap> nextHeap;
    RADRAY_DX_CHECK(_device->_device->CreateDescriptorHeap(&_desc, IID_PPV_ARGS(nextHeap.GetAddressOf())));
    D3D12_CPU_DESCRIPTOR_HANDLE nextCpuStart = nextHeap->GetCPUDescriptorHandleForHeapStart();
    _device->_device->CopyDescriptorsSimple(_desc.NumDescriptors, nextCpuStart, _cpuStart, _desc.Type);
    _desc = nextDesc;
    _heap = nextHeap;
    _cpuStart = nextCpuStart;
    if (_desc.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) {
        _gpuStart = nextHeap->GetGPUDescriptorHandleForHeapStart();
    }
    RADRAY_DEBUG_LOG("D3D12 expand DescHeap type={} isGpuVis={} incrementSize={} length={} all={}(bytes)",
                     _desc.Type,
                     ((_desc.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) == D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE),
                     _incrementSize,
                     _desc.NumDescriptors, UINT64(_desc.NumDescriptors) * _incrementSize);
}

}  // namespace radray::render::d3d12
