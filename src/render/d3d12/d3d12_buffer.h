#pragma once

#include <radray/render/resource.h>
#include "d3d12_helper.h"

namespace radray::render::d3d12 {

class BufferD3D12 : public Buffer {
public:
    BufferD3D12(
        ComPtr<ID3D12Resource> buf,
        ComPtr<D3D12MA::Allocation> alloc,
        D3D12_RESOURCE_STATES initState,
        ResourceType type) noexcept;
    ~BufferD3D12() noexcept override = default;

    bool IsValid() const noexcept override;
    void Destroy() noexcept override;
    ResourceType GetType() const noexcept override;
    ResourceStates GetInitState() const noexcept override;

    uint64_t GetSize() const noexcept override;

    Nullable<void> Map(uint64_t offset, uint64_t size) noexcept override;

    void Unmap() noexcept override;

public:
    ComPtr<ID3D12Resource> _buf;
    ComPtr<D3D12MA::Allocation> _alloc;
    D3D12_RESOURCE_DESC _desc;
    D3D12_RESOURCE_STATES _initState;
    ResourceType _type;
};

class BufferViewD3D12 : public BufferView {
public:
    BufferViewD3D12(
        BufferD3D12* buffer,
        DescriptorHeap* heap,
        UINT heapIndex,
        ResourceType type,
        DXGI_FORMAT format,
        uint32_t count,
        uint64_t offset,
        uint32_t stride) noexcept
        : _buffer(buffer),
          _heap(heap),
          _heapIndex(heapIndex),
          _type(type),
          _format(format),
          _count(count),
          _offset(offset),
          _stride(stride) {}
    ~BufferViewD3D12() noexcept override = default;

    bool IsValid() const noexcept override;
    void Destroy() noexcept override;

public:
    BufferD3D12* _buffer{nullptr};
    DescriptorHeap* _heap{nullptr};
    UINT _heapIndex{std::numeric_limits<UINT>::max()};
    ResourceType _type;
    DXGI_FORMAT _format;
    uint32_t _count;
    uint64_t _offset;
    uint32_t _stride;
};

}  // namespace radray::render::d3d12
