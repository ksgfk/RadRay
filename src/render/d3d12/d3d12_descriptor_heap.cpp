#include "d3d12_descriptor_heap.h"

#include <cmath>

#include "d3d12_device.h"

namespace radray::render::d3d12 {

DescriptorHeap1::DescriptorHeap1(
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

ID3D12DescriptorHeap* DescriptorHeap1::Get() const noexcept {
    return _heap.Get();
}

D3D12_DESCRIPTOR_HEAP_TYPE DescriptorHeap1::GetHeapType() const noexcept {
    return _desc.Type;
}

UINT DescriptorHeap1::GetLength() const noexcept {
    return _desc.NumDescriptors;
}

bool DescriptorHeap1::IsShaderVisible() const noexcept {
    return (_desc.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) == D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
}

D3D12_GPU_DESCRIPTOR_HANDLE DescriptorHeap1::HandleGpu(UINT index) const noexcept {
    return {_gpuStart.ptr + UINT64(index) * UINT64(_incrementSize)};
}

D3D12_CPU_DESCRIPTOR_HANDLE DescriptorHeap1::HandleCpu(UINT index) const noexcept {
    return {_cpuStart.ptr + UINT64(index) * UINT64(_incrementSize)};
}

void DescriptorHeap1::Create(ID3D12Resource* resource, const D3D12_UNORDERED_ACCESS_VIEW_DESC& desc, UINT index) noexcept {
    _device->CreateUnorderedAccessView(resource, nullptr, &desc, HandleCpu(index));
}

void DescriptorHeap1::Create(ID3D12Resource* resource, const D3D12_SHADER_RESOURCE_VIEW_DESC& desc, UINT index) noexcept {
    _device->CreateShaderResourceView(resource, &desc, HandleCpu(index));
}

void DescriptorHeap1::Create(const D3D12_CONSTANT_BUFFER_VIEW_DESC& desc, UINT index) noexcept {
    _device->CreateConstantBufferView(&desc, HandleCpu(index));
}

void DescriptorHeap1::Create(ID3D12Resource* resource, const D3D12_RENDER_TARGET_VIEW_DESC& desc, UINT index) noexcept {
    _device->CreateRenderTargetView(resource, &desc, HandleCpu(index));
}

void DescriptorHeap1::Create(ID3D12Resource* resource, const D3D12_DEPTH_STENCIL_VIEW_DESC& desc, UINT index) noexcept {
    _device->CreateDepthStencilView(resource, &desc, HandleCpu(index));
}

void DescriptorHeap1::Create(const D3D12_SAMPLER_DESC& desc, UINT index) noexcept {
    _device->CreateSampler(&desc, HandleCpu(index));
}

void DescriptorHeap1::CopyTo(UINT start, UINT count, DescriptorHeap* dst, UINT dstStart) noexcept {
    _device->CopyDescriptorsSimple(
        count,
        dst->HandleCpu(dstStart),
        HandleCpu(start),
        _desc.Type);
}

DescriptorHeapView CpuDescriptorAllocator::Allocate(uint32_t count) noexcept {
    if (count == 0) {
        return DescriptorHeapView{nullptr, 0, 0};
    }
    for (auto iter = _sizeQuery.lower_bound(count); iter != _sizeQuery.end(); iter++) {
        CpuDescriptorAllocator::Block* block = iter->second;
        std::optional<uint64_t> start = block->_allocator.Allocate(count);
        if (start.has_value()) {
            block->_used += count;
            _sizeQuery.erase(iter);
            if (block->GetFreeSize() > 0) {
                _sizeQuery.emplace(block->GetFreeSize(), block);
            }
            return DescriptorHeapView{block->_heap.get(), static_cast<UINT>(start.value()), count};
        }
    }
    {
        D3D12_DESCRIPTOR_HEAP_DESC desc{
            _type,
            std::bit_ceil(std::max(count, _basicSize)),
            D3D12_DESCRIPTOR_HEAP_FLAG_NONE,
            0};
        auto block = std::make_unique<CpuDescriptorAllocator::Block>(_device, desc);
        Block* blockPtr = block.get();
        auto [newIter, isInsert] = _blocks.emplace(blockPtr, std::move(block));
        RADRAY_ASSERT(isInsert);
        uint64_t start = blockPtr->_allocator.Allocate(count).value();
        blockPtr->_used += count;
        if (blockPtr->GetFreeSize() > 0) {
            _sizeQuery.emplace(blockPtr->GetFreeSize(), newIter->second.get());
        }
        return DescriptorHeapView{blockPtr->_heap.get(), static_cast<UINT>(start), count};
    }
}

void CpuDescriptorAllocator::Free(DescriptorHeapView view) noexcept {
    auto iter = _blocks.find(view.Heap);
    if (iter == _blocks.end()) {
        RADRAY_ABORT("D3D12 CpuDescriptorAllocator::Free invalid heap");
        return;
    }
    Block* block = iter->second.get();
    block->_allocator.Destroy(view.Start);
    block->_used -= view.Length;
    auto [qBegin, qEnd] = _sizeQuery.equal_range(block->GetFreeSize());
    for (auto it = qBegin; it != qEnd; it++) {
        if (it->second == block) {
            _sizeQuery.erase(it);
            break;
        }
    }
    if (block->GetFreeSize() > 0) {
        _sizeQuery.emplace(block->GetFreeSize(), block);
    }
}

CpuDescriptorAllocator::Block::Block(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_DESC desc) noexcept
    : _heap(std::make_unique<DescriptorHeap1>(device, desc)),
      _allocator(desc.NumDescriptors),
      _used(0) {}

uint64_t CpuDescriptorAllocator::Block::GetFreeSize() const noexcept {
    return _heap->GetLength() - _used;
}

DescriptorHeap::DescriptorHeap(
    ID3D12Device* device,
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
    RADRAY_DX_CHECK(_device->CreateDescriptorHeap(&_desc, IID_PPV_ARGS(_heap.GetAddressOf())));
    _cpuStart = _heap->GetCPUDescriptorHandleForHeapStart();
    if (isShaderVisible) {
        _gpuStart = _heap->GetGPUDescriptorHandleForHeapStart();
    } else {
        _gpuStart = {0};
    }
    _incrementSize = _device->GetDescriptorHandleIncrementSize(_desc.Type);
    RADRAY_DEBUG_LOG(
        "D3D12 create DescHeap type={} isGpuVis={} incrementSize={} length={} all={}(bytes)",
        _desc.Type,
        isShaderVisible,
        _incrementSize, length, UINT64(length) * _incrementSize);
}

UINT DescriptorHeap::Allocate() noexcept {
    UINT result;
    if (_empty.empty()) {
        if (_allocIndex == _desc.NumDescriptors) {
            ExpandCapacity(_desc.NumDescriptors + 1);
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
    if (_desc.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) {
        return;
    }
    if (_desc.NumDescriptors != _initLength) {
        _desc.NumDescriptors = _initLength;
        _heap.Reset();
        RADRAY_DX_CHECK(_device->CreateDescriptorHeap(&_desc, IID_PPV_ARGS(_heap.GetAddressOf())));
        _cpuStart = _heap->GetCPUDescriptorHandleForHeapStart();
    }
}

UINT DescriptorHeap::AllocateRange(UINT count) noexcept {
    // TODO: 写的什么jb玩意, 有空了重写
    if (count == 0) {
        return std::numeric_limits<UINT>::max();
    }
    if (_allocIndex + count >= _desc.NumDescriptors) {
        ExpandCapacity(_allocIndex + count);
    }
    std::sort(_empty.begin(), _empty.end());
    UINT continuous = 0;
    size_t i = 0;
    for (; i < _empty.size(); i++) {
        UINT v = _empty[i];
        continuous++;
        if (i != 0) {
            UINT last = _empty[i - 1];
            if (last != v - 1) {
                continuous = 1;
            }
        }
        if (continuous >= count) {
            break;
        }
    }
    if (continuous >= count) {
        size_t start = i - count + 1;
        size_t end = i + 1;
        UINT result = _empty[start];
        _empty.erase(_empty.begin() + start, _empty.begin() + end);
        return result;
    }
    UINT v = 0, start = _allocIndex;
    if (!_empty.empty() && _empty[_empty.size() - 1] == (_allocIndex - 1) && continuous > 0) {
        v = continuous;
        start = _allocIndex - v;
        auto begin = std::lower_bound(_empty.begin(), _empty.end(), start);
        _empty.erase(begin, _empty.end());
    }
    _allocIndex += count - v;
    return start;
}

void DescriptorHeap::RecycleRange(UINT start, UINT count) noexcept {
    for (UINT i = 0; i < count; i++) {
        _empty.emplace_back(start + i);
    }
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

void DescriptorHeap::ExpandCapacity(UINT need) noexcept {
    if (_desc.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) {
        RADRAY_ABORT("DescriptorHeap cannot expand GPU visible heap");
        return;
    }
    UINT now = _desc.NumDescriptors;
    UINT next = static_cast<UINT>(std::min(std::max(UINT64(need), static_cast<UINT64>(UINT64(now) * 1.5)), UINT64(std::numeric_limits<UINT>::max())));
    if (now >= next) {
        RADRAY_ABORT("DescriptorHeap expand failed");
        return;
    }
    D3D12_DESCRIPTOR_HEAP_DESC nextDesc{
        .Type = _desc.Type,
        .NumDescriptors = next,
        .Flags = _desc.Flags,
        .NodeMask = _desc.NodeMask};
    ComPtr<ID3D12DescriptorHeap> nextHeap;
    RADRAY_DX_CHECK(_device->CreateDescriptorHeap(&_desc, IID_PPV_ARGS(nextHeap.GetAddressOf())));
    D3D12_CPU_DESCRIPTOR_HANDLE nextCpuStart = nextHeap->GetCPUDescriptorHandleForHeapStart();
    _device->CopyDescriptorsSimple(_desc.NumDescriptors, nextCpuStart, _cpuStart, _desc.Type);
    _desc = nextDesc;
    _heap = nextHeap;
    _cpuStart = nextCpuStart;
    if (_desc.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) {
        _gpuStart = nextHeap->GetGPUDescriptorHandleForHeapStart();
    }
    RADRAY_DEBUG_LOG(
        "D3D12 expand DescHeap type={} isGpuVis={} incrementSize={} length={} all={}(bytes)",
        _desc.Type,
        ((_desc.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) == D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE),
        _incrementSize,
        _desc.NumDescriptors, UINT64(_desc.NumDescriptors) * _incrementSize);
}

}  // namespace radray::render::d3d12
