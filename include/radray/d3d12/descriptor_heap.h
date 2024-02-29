#pragma once

#include <radray/d3d12/utility.h>

namespace radray::d3d12 {

// https://zhuanlan.zhihu.com/p/149721407
// https://zhuanlan.zhihu.com/p/572693888
// https://zhuanlan.zhihu.com/p/682722245
// http://diligentgraphics.com/diligent-engine/architecture/d3d12/managing-descriptor-heaps/

class Device;

class DescriptorHeap {
public:
    DescriptorHeap(Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type, uint32 numDescriptors, bool shaderVisible) noexcept;
    DescriptorHeap(DescriptorHeap&&) noexcept = default;
    DescriptorHeap& operator=(DescriptorHeap&&) noexcept = default;
    DescriptorHeap(const DescriptorHeap&) = delete;
    DescriptorHeap& operator=(const DescriptorHeap&) = delete;
    ~DescriptorHeap() noexcept = default;

    D3D12_DESCRIPTOR_HEAP_TYPE GetType() const noexcept { return _desc.Type; }
    ID3D12DescriptorHeap* GetHeap() const noexcept { return _descHeap.Get(); }
    D3D12_GPU_DESCRIPTOR_HANDLE HandleGPU(uint64 index) const noexcept;
    D3D12_CPU_DESCRIPTOR_HANDLE HandleCPU(uint64 index) const noexcept;
    void CreateUav(ID3D12Resource* resource, const D3D12_UNORDERED_ACCESS_VIEW_DESC& pDesc, uint64 index) const noexcept;
    void CreateSrv(ID3D12Resource* resource, const D3D12_SHADER_RESOURCE_VIEW_DESC& pDesc, uint64 index) const noexcept;
    void CreateRtv(ID3D12Resource* resource, const D3D12_RENDER_TARGET_VIEW_DESC& pDesc, uint64 index) const noexcept;
    void CreateDsv(ID3D12Resource* resource, const D3D12_DEPTH_STENCIL_VIEW_DESC& pDesc, uint64 index) const noexcept;
    void CreateSampler(const D3D12_SAMPLER_DESC& desc, uint64 index) const noexcept;

private:
    Device* _device;
    ComPtr<ID3D12DescriptorHeap> _descHeap;
    D3D12_DESCRIPTOR_HEAP_DESC _desc;
    D3D12_CPU_DESCRIPTOR_HANDLE _cpuHeapStart;
    D3D12_GPU_DESCRIPTOR_HANDLE _gpuHeapStart;
    uint32 _handleIncrementSize;
};

}  // namespace radray::d3d12
