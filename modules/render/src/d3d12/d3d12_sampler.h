#pragma once

#include <radray/render/sampler.h>
#include "d3d12_helper.h"
#include "d3d12_descriptor_heap.h"

namespace radray::render::d3d12 {

class SamplerD3D12 : public Sampler {
public:
    SamplerD3D12(
        DescriptorHeapView view,
        CpuDescriptorAllocator* alloc,
        const SamplerDescriptor& desc,
        const D3D12_SAMPLER_DESC& rawDesc) noexcept;
    ~SamplerD3D12() noexcept override;

    bool IsValid() const noexcept override;
    void Destroy() noexcept override;

    SamplerDescriptor GetDesc() const noexcept override;

public:
    DescriptorHeapView _heapView;
    CpuDescriptorAllocator* _heapAlloc;
    SamplerDescriptor _desc;
    D3D12_SAMPLER_DESC _rawDesc;
};

}  // namespace radray::render::d3d12
