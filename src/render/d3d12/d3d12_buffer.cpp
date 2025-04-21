#include "d3d12_buffer.h"

#include "d3d12_descriptor_heap.h"

namespace radray::render::d3d12 {

BufferD3D12::BufferD3D12(
    ComPtr<ID3D12Resource> buf,
    ComPtr<D3D12MA::Allocation> alloc,
    D3D12_RESOURCE_STATES initState,
    ResourceType type) noexcept
    : _buf(std::move(buf)),
      _alloc(std::move(alloc)),
      _initState(initState),
      _type(type) {
    _desc = _buf->GetDesc();
    _gpuAddr = _buf->GetGPUVirtualAddress();
}

bool BufferD3D12::IsValid() const noexcept { return _buf != nullptr; }

void BufferD3D12::Destroy() noexcept {
    _buf = nullptr;
    _alloc = nullptr;
}

ResourceType BufferD3D12::GetType() const noexcept {
    return _type;
}

uint64_t BufferD3D12::GetSize() const noexcept {
    return _desc.Width;
}

ResourceStates BufferD3D12::GetInitState() const noexcept {
    return MapType(_initState);
}

Nullable<void> BufferD3D12::Map(uint64_t offset, uint64_t size) noexcept {
    D3D12_RANGE range{offset, offset + size};
    void* ptr = nullptr;
    if (HRESULT hr = _buf->Map(0, &range, &ptr);
        SUCCEEDED(hr)) {
        return ptr;
    } else {
        RADRAY_ERR_LOG("cannot map buffer, reason={} (code:{})", GetErrorName(hr), hr);
        return nullptr;
    }
}

void BufferD3D12::Unmap() noexcept {
    _buf->Unmap(0, nullptr);
}

BufferViewD3D12::~BufferViewD3D12() noexcept {
    Destroy();
}

bool BufferViewD3D12::IsValid() const noexcept {
    return _desc.buffer != nullptr && _desc.heapView.IsValid();
}

void BufferViewD3D12::Destroy() noexcept {
    if (!IsValid()) {
        return;
    }
    _desc.heapAlloc->Destroy(_desc.heapView);
    _desc.heapView = DescriptorHeapView::Invalid();
    _desc.buffer = nullptr;
}

void BufferViewD3D12::CopyTo(DescriptorHeap* dst, UINT dstStart) noexcept {
    _desc.heapView.Heap->CopyTo(_desc.heapView.Start, 1, dst, dstStart);
}

}  // namespace radray::render::d3d12
