#include "d3d12_buffer.h"

namespace radray::render::d3d12 {

BufferD3D12::BufferD3D12(
    ComPtr<ID3D12Resource> buf,
    ComPtr<D3D12MA::Allocation> alloc,
    D3D12_RESOURCE_STATES initState,
    ResourceType type) noexcept
    : _buf(std::move(buf)),
      _alloc(std::move(alloc)),
      _initState(initState),
      _type(type) {
    _desc = _buf->GetDesc();
}

bool BufferD3D12::IsValid() const noexcept { return _buf != nullptr; }

void BufferD3D12::Destroy() noexcept {
    _buf = nullptr;
    _alloc = nullptr;
}

ResourceType BufferD3D12::GetType() const noexcept {
    return _type;
}

ResourceStates BufferD3D12::GetInitState() const noexcept {
    return MapType(_initState);
}

}  // namespace radray::render::d3d12
