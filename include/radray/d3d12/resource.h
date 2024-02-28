#pragma once

#include <memory>
#include <vector>
#include <unordered_map>
#include <radray/d3d12/utility.h>

namespace radray::d3d12 {

class Device;
class IGpuHeapAllocator;

struct GpuHeapInfo {
    ID3D12Heap* heap;
    uint64 offset;
};

class IGpuHeapAllocation {
public:
    virtual ~IGpuHeapAllocation() noexcept = default;

    virtual GpuHeapInfo GetHeap() noexcept = 0;
};

class IGpuHeapAllocator {
public:
    virtual ~IGpuHeapAllocator() noexcept = default;

    virtual std::shared_ptr<IGpuHeapAllocation> AllocBufferHeap(uint64_t byteSize, D3D12_HEAP_TYPE heapType, D3D12_HEAP_FLAGS extraFlags = D3D12_HEAP_FLAG_NONE) noexcept = 0;
    virtual std::shared_ptr<IGpuHeapAllocation> AllocTextureHeap(uint64_t byteSize, bool isRenderTexture, D3D12_HEAP_FLAGS extraFlags = D3D12_HEAP_FLAG_NONE) noexcept = 0;

    static std::unique_ptr<IGpuHeapAllocator> MakeDefaultAllocator(Device* device) noexcept;
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

class ResourceStateTracker {
public:
    void Track(const Resource* resource, D3D12_RESOURCE_STATES state);
    void Update(ID3D12GraphicsCommandList* cmd);
    void Restore(ID3D12GraphicsCommandList* cmd);

private:
    struct State {
        D3D12_RESOURCE_STATES lastState;
        D3D12_RESOURCE_STATES currState;
    };

    void ExecuteStateMap();
    void RestoreStateMap();

    std::vector<D3D12_RESOURCE_BARRIER> _states;
    std::unordered_map<const Resource*, State> _stateMap;
};

}  // namespace radray::d3d12
