#pragma once

#include <radray/render/resource.h>
#include "d3d12_helper.h"
#include "d3d12_descriptor_heap.h"

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
    D3D12_GPU_VIRTUAL_ADDRESS _gpuAddr;
    D3D12_RESOURCE_STATES _initState;
    ResourceType _type;
};

struct BufferViewD3D12Desc {
    BufferD3D12* buffer;
    DescriptorHeapView heapView;
    CpuDescriptorAllocator* heapAlloc;
    ResourceType type;
    DXGI_FORMAT format;
    uint32_t count;
    uint64_t offset;
    uint32_t stride;
};

class BufferViewD3D12 : public ResourceViewD3D12 {
public:
    explicit BufferViewD3D12(BufferViewD3D12Desc desc) noexcept
        : ResourceViewD3D12(ResourceView::Type::Buffer),
          _desc(desc) {}
    ~BufferViewD3D12() noexcept override;

    bool IsValid() const noexcept override;
    void Destroy() noexcept override;

public:
    BufferViewD3D12Desc _desc;
};

}  // namespace radray::render::d3d12
