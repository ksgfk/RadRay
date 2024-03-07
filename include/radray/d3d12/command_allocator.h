#pragma once

#include <radray/allocator.h>
#include <radray/d3d12/command_list.h>
#include <radray/d3d12/resource.h>

namespace radray::d3d12 {

class Device;
class CommandQueue;
class DescriptorHeap;
class UploadBuffer;

class CommandAllocator {
public:
    CommandAllocator(Device* device, D3D12_COMMAND_LIST_TYPE type) noexcept;

    void Execute(CommandQueue* queue, ID3D12Fence* fence, uint64 fenceIndex);
    void Reset();

    Device* device;
    ComPtr<ID3D12CommandAllocator> alloc;
    std::unique_ptr<CommandList> cmd;
    D3D12_COMMAND_LIST_TYPE type;

private:
    class CpuDescriptorHeapAllocator : public IAllocator<DescriptorHeap*> {
    public:
        CpuDescriptorHeapAllocator(Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type) noexcept;
        ~CpuDescriptorHeapAllocator() noexcept override = default;
        DescriptorHeap* Allocate(uint64 size) noexcept override;
        void Destroy(DescriptorHeap* handle) noexcept override;
        Device* device;
        D3D12_DESCRIPTOR_HEAP_TYPE type;
    };
    class GpuUploadBufferAllocator : public IAllocator<UploadBuffer*> {
    public:
        GpuUploadBufferAllocator(Device* device) noexcept;
        ~GpuUploadBufferAllocator() noexcept override = default;
        UploadBuffer* Allocate(uint64 size) noexcept override;
        void Destroy(UploadBuffer* handle) noexcept override;
        Device* device;
    };
    CpuDescriptorHeapAllocator _rtvAllocator;
    CpuDescriptorHeapAllocator _dsvAllocator;
    GpuUploadBufferAllocator _uploadAllocator;

public:
    LinearAllocator<DescriptorHeap*> rtvHeap;
    LinearAllocator<DescriptorHeap*> dsvHeap;
    LinearAllocator<UploadBuffer*> uploadAlloc;

public:
    uint64 lastExecuteFenceIndex;
    ResourceStateTracker stateTracker;
};

}  // namespace radray::d3d12
