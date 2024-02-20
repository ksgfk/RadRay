#pragma once

#include <radray/d3d12/utility.h>

namespace radray::d3d12 {

class Device;
class IGpuHeapAllocator;

class GpuHeapAllocation {
public:
    ~GpuHeapAllocation() noexcept;

private:
    GpuHeapAllocation(IGpuHeapAllocator* alloc, uint64 handle) noexcept;

    IGpuHeapAllocator* _alloc;
    uint64 _handle;

    friend class IGpuHeapAllocator;
};

class IGpuHeapAllocator {
public:
    virtual ~IGpuHeapAllocator() noexcept = default;

    virtual GpuHeapAllocation AllocBufferHeap(
        Device* device,
        uint64_t byteSize,
        D3D12_HEAP_TYPE heapType,
        D3D12_HEAP_FLAGS extraFlags = D3D12_HEAP_FLAG_NONE) = 0;

    virtual GpuHeapAllocation AllocTextureHeap(
        Device* device,
        uint64_t byteSize,
        bool isRenderTexture,
        D3D12_HEAP_FLAGS extraFlags = D3D12_HEAP_FLAG_NONE) = 0;

    virtual void FreeHeap(GpuHeapAllocation& alloc) = 0;
};

class Resource {
public:
    Resource(Device* device) noexcept;
    virtual ~Resource() noexcept = default;
    Resource(Resource&&) noexcept = default;
    Resource(const Resource&) noexcept = delete;
    Resource& operator=(Resource&&) noexcept = default;
    Resource& operator=(const Resource&) noexcept = delete;

    virtual ID3D12Resource* GetResource() const noexcept { return nullptr; }
    virtual D3D12_RESOURCE_STATES GetInitState() const noexcept { return D3D12_RESOURCE_STATE_COMMON; }

public:
    Device* device;
};

}  // namespace radray::d3d12
