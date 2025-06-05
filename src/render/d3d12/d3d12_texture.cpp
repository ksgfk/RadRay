#include "d3d12_texture.h"

#include "d3d12_device.h"
#include "d3d12_descriptor_heap.h"
#include "d3d12_buffer.h"

namespace radray::render::d3d12 {

TextureD3D12::TextureD3D12(
    DeviceD3D12* device,
    ComPtr<ID3D12Resource> tex,
    ComPtr<D3D12MA::Allocation> alloc,
    const D3D12_RESOURCE_STATES& initState,
    ResourceType type) noexcept
    : _device(device),
      _tex(std::move(tex)),
      _alloc(std::move(alloc)),
      _initState(initState),
      _type(type) {
    _desc = _tex->GetDesc();
}

bool TextureD3D12::IsValid() const noexcept { return _tex != nullptr; }

void TextureD3D12::Destroy() noexcept {
    _tex = nullptr;
    _alloc = nullptr;
}

ResourceType TextureD3D12::GetType() const noexcept {
    return _type;
}

ResourceStates TextureD3D12::GetInitState() const noexcept {
    return MapType(_initState);
}

static void TextureViewD3D12DestroyImpl(TextureViewD3D12& that) noexcept {
    if (that.IsValid()) {
        that._desc.heapAlloc->Destroy(that._desc.heapView);
        that._desc.heapView = DescriptorHeapView::Invalid();
        that._desc.texture = nullptr;
    }
}

TextureViewD3D12::~TextureViewD3D12() noexcept {
    TextureViewD3D12DestroyImpl(*this);
}

bool TextureViewD3D12::IsValid() const noexcept {
    return _desc.texture != nullptr && _desc.heapView.IsValid();
}

void TextureViewD3D12::Destroy() noexcept {
    TextureViewD3D12DestroyImpl(*this);
}

void TextureViewD3D12::CopyTo(DescriptorHeap* dst, UINT dstStart) noexcept {
    _desc.heapView.Heap->CopyTo(_desc.heapView.Start, 1, dst, dstStart);
}

}  // namespace radray::render::d3d12
