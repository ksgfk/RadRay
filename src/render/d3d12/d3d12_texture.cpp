#include "d3d12_texture.h"

namespace radray::render::d3d12 {

TextureD3D12::TextureD3D12(
    ComPtr<ID3D12Resource> tex,
    ComPtr<D3D12MA::Allocation> alloc,
    const D3D12_RESOURCE_STATES& initState,
    ResourceType type) noexcept
    : _tex(std::move(tex)),
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

}  // namespace radray::render::d3d12