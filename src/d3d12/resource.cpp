#include <radray/d3d12/resource.h>

#define D3D12MA_D3D12_HEADERS_ALREADY_INCLUDED
#include <D3D12MemAlloc.h>
#include <radray/d3d12/device.h>

namespace radray::d3d12 {

class DefaultGpuHeapAllocation final : public IGpuHeapAllocation {
public:
    DefaultGpuHeapAllocation(ComPtr<D3D12MA::Allocation>&& alloc) : _alloc(std::move(alloc)) {}
    ~DefaultGpuHeapAllocation() override = default;

    GpuHeapInfo GetHeap() noexcept override {
        return {_alloc->GetHeap(), _alloc->GetOffset()};
    }

private:
    ComPtr<D3D12MA::Allocation> _alloc;
};

class DefaultGpuHeapAllocator final : public IGpuHeapAllocator {
public:
    DefaultGpuHeapAllocator(Device* device) noexcept;
    ~DefaultGpuHeapAllocator() noexcept override = default;

    std::shared_ptr<IGpuHeapAllocation> AllocBufferHeap(uint64_t byteSize, D3D12_HEAP_TYPE heapType, D3D12_HEAP_FLAGS extraFlags = D3D12_HEAP_FLAG_NONE) noexcept override;
    std::shared_ptr<IGpuHeapAllocation> AllocTextureHeap(uint64_t byteSize, bool isRenderTexture, D3D12_HEAP_FLAGS extraFlags = D3D12_HEAP_FLAG_NONE) noexcept override;

private:
    ComPtr<D3D12MA::Allocator> _alloc;
};

DefaultGpuHeapAllocator::DefaultGpuHeapAllocator(Device* device) noexcept {
    D3D12MA::ALLOCATOR_DESC desc{
        .Flags = D3D12MA::ALLOCATOR_FLAG_NONE,
        .pDevice = device->device.Get(),
        .PreferredBlockSize = 0,
        .pAllocationCallbacks = nullptr,
        .pAdapter = device->adapter.Get()};
    ThrowIfFailed(D3D12MA::CreateAllocator(&desc, _alloc.GetAddressOf()));
}

std::shared_ptr<IGpuHeapAllocation> DefaultGpuHeapAllocator::AllocBufferHeap(
    uint64_t byteSize,
    D3D12_HEAP_TYPE heapType,
    D3D12_HEAP_FLAGS extraFlags) noexcept {
    D3D12MA::ALLOCATION_DESC desc{
        .Flags = D3D12MA::ALLOCATION_FLAGS::ALLOCATION_FLAG_STRATEGY_BEST_FIT,
        .HeapType = heapType,
        .ExtraHeapFlags = extraFlags,
        .CustomPool = nullptr,
        .pPrivateData = nullptr};
    D3D12_RESOURCE_ALLOCATION_INFO info{
        .SizeInBytes = CalcPlacedOffsetAlignment(byteSize),
        .Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT};
    ComPtr<D3D12MA::Allocation> alloc;
    ThrowIfFailed(_alloc->AllocateMemory(&desc, &info, alloc.GetAddressOf()));
    return std::make_shared<DefaultGpuHeapAllocation>(std::move(alloc));
}

std::shared_ptr<IGpuHeapAllocation> DefaultGpuHeapAllocator::AllocTextureHeap(
    uint64_t byteSize,
    bool isRenderTexture,
    D3D12_HEAP_FLAGS extraFlags) noexcept {
    D3D12_HEAP_FLAGS texFlag = isRenderTexture ? D3D12_HEAP_FLAG_ALLOW_ONLY_RT_DS_TEXTURES : D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES;
    D3D12MA::ALLOCATION_DESC desc{
        .Flags = D3D12MA::ALLOCATION_FLAGS::ALLOCATION_FLAG_STRATEGY_BEST_FIT,
        .HeapType = D3D12_HEAP_TYPE_DEFAULT,
        .ExtraHeapFlags = texFlag | extraFlags,
        .CustomPool = nullptr,
        .pPrivateData = nullptr};
    D3D12_RESOURCE_ALLOCATION_INFO info{
        .SizeInBytes = byteSize,
        .Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT};
    D3D12MA::Allocation* alloc;
    ThrowIfFailed(_alloc->AllocateMemory(&desc, &info, &alloc));
    return std::make_shared<DefaultGpuHeapAllocation>(std::move(alloc));
}

std::unique_ptr<IGpuHeapAllocator> IGpuHeapAllocator::MakeDefaultAllocator(Device* device) noexcept {
    return std::make_unique<DefaultGpuHeapAllocator>(device);
}

Resource::Resource(Device* device) noexcept : device(device) {}

void ResourceStateTracker::Track(const Resource* resource, D3D12_RESOURCE_STATES state) {
    auto&& [iter, isInsert] = _stateMap.try_emplace(resource, State{});
    if (isInsert) {
        auto&& s = iter->second;
        s.lastState = resource->GetInitState();
        s.currState = state;
    } else {
        auto&& s = iter->second;
        s.currState = state;
    }
}

void ResourceStateTracker::Update(ID3D12GraphicsCommandList* cmd) {
    ExecuteStateMap();
    if (!_states.empty()) {
        cmd->ResourceBarrier(_states.size(), _states.data());
        _states.clear();
    }
}

void ResourceStateTracker::Restore(ID3D12GraphicsCommandList* cmd) {
    RestoreStateMap();
    if (!_states.empty()) {
        cmd->ResourceBarrier(_states.size(), _states.data());
        _states.clear();
    }
}

void ResourceStateTracker::ExecuteStateMap() {
    for (auto&& i : _stateMap) {
        auto res = i.first;
        auto&& s = i.second;
        if (s.currState != s.lastState) {
            D3D12_RESOURCE_BARRIER& transBarrier = _states.emplace_back();
            transBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            transBarrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            transBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            transBarrier.Transition.pResource = res->GetResource();
            transBarrier.Transition.StateBefore = s.lastState;
            transBarrier.Transition.StateAfter = s.currState;
        }
        s.lastState = s.currState;
    }
}

void ResourceStateTracker::RestoreStateMap() {
    for (auto&& i : _stateMap) {
        auto res = i.first;
        auto&& s = i.second;
        s.currState = res->GetInitState();
        if (s.currState != s.lastState) {
            D3D12_RESOURCE_BARRIER& transBarrier = _states.emplace_back();
            transBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            transBarrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            transBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            transBarrier.Transition.pResource = res->GetResource();
            transBarrier.Transition.StateBefore = s.lastState;
            transBarrier.Transition.StateAfter = s.currState;
        }
    }
    _stateMap.clear();
}

}  // namespace radray::d3d12
