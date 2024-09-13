#include "d3d12_texture.h"

#include "d3d12_device.h"

namespace radray::rhi::d3d12 {

Texture::Texture(
    Device* device,
    const D3D12_RESOURCE_DESC& resDesc,
    D3D12_RESOURCE_STATES initState,
    const D3D12MA::ALLOCATION_DESC& allocDesc)
    : desc(resDesc),
      initState(initState) {
    RADRAY_DX_FTHROW(device->resourceAlloc->CreateResource(
        &allocDesc,
        &resDesc,
        initState,
        nullptr,
        alloc.GetAddressOf(),
        IID_PPV_ARGS(texture.GetAddressOf())));
}

Texture::Texture(
    const ComPtr<ID3D12Resource>& res,
    D3D12_RESOURCE_STATES initState)
    : desc(res->GetDesc()),
      initState(initState),
      texture(res),
      alloc{} {}

TextureView::TextureView(DescriptorHeap* heap, Texture* tex, const ViewDesc& desc)
    : desc(desc),
      heap(heap),
      tex(tex),
      index(std::numeric_limits<UINT>::max()) {
    auto allocIdx = heap->Allocate();
    auto guard = MakeScopeGuard([heap, allocIdx]() { heap->Recycle(allocIdx); });
    std::visit([heap, tex, allocIdx](auto&& value) { heap->Create(tex->texture.Get(), value, allocIdx); }, desc);
    guard.Dismiss();
    index = allocIdx;
}

TextureView::~TextureView() noexcept {
    if (index != std::numeric_limits<UINT>::max()) {
        heap->Recycle(index);
        index = std::numeric_limits<UINT>::max();
    }
}

}  // namespace radray::rhi::d3d12
