#pragma once

#include <radray/allocator.h>
#include <radray/d3d12/command_list.h>
#include <radray/d3d12/resource.h>

namespace radray::d3d12 {

class Device;
class CommandQueue;
class DescriptorHeap;

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
        virtual DescriptorHeap* Allocate(uint64 size) noexcept override;
        virtual void Destroy(DescriptorHeap* handle) noexcept override;
        Device* device;
        D3D12_DESCRIPTOR_HEAP_TYPE type;
    };
    CpuDescriptorHeapAllocator _rtvAllocator;
    CpuDescriptorHeapAllocator _dsvAllocator;

public:
    LinearAllocator<DescriptorHeap*> rtvHeap;
    LinearAllocator<DescriptorHeap*> dsvHeap;

public:
    uint64 lastExecuteFenceIndex;
    ResourceStateTracker stateTracker;
};

}  // namespace radray::d3d12
