#include "d3d12_sampler.h"

namespace radray::render::d3d12 {

static void SamplerD3D12DestroyImpl(SamplerD3D12& that) noexcept {
    if (!that._heapView.IsValid()) {
        return;
    }
    that._heapAlloc->Destroy(that._heapView);
    that._heapView = DescriptorHeapView::Invalid();
    that._heapAlloc = nullptr;
}

SamplerD3D12::SamplerD3D12(
    DescriptorHeapView view,
    CpuDescriptorAllocator* alloc,
    const SamplerDescriptor& desc,
    const D3D12_SAMPLER_DESC& rawDesc) noexcept
    : _heapView(view),
      _heapAlloc(alloc),
      _desc(desc),
      _rawDesc(rawDesc) {}

SamplerD3D12::~SamplerD3D12() noexcept {
    SamplerD3D12DestroyImpl(*this);
}

bool SamplerD3D12::IsValid() const noexcept {
    return _heapView.IsValid();
}

void SamplerD3D12::Destroy() noexcept {
    SamplerD3D12DestroyImpl(*this);
}

SamplerDescriptor SamplerD3D12::GetDesc() const noexcept { return _desc; }

}  // namespace radray::render::d3d12
