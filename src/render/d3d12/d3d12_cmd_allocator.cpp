#include "d3d12_cmd_allocator.h"

#include "d3d12_device.h"
#include "d3d12_cmd_list.h"

namespace radray::render::d3d12 {

void CmdAllocatorD3D12::Destroy() noexcept {
    _cmdAlloc = nullptr;
}

std::optional<radray::shared_ptr<CommandBuffer>> CmdAllocatorD3D12::CreateCommandBuffer() noexcept {
    ComPtr<ID3D12GraphicsCommandList> list;
    if (HRESULT hr = _device->_device->CreateCommandList(0, _type, _cmdAlloc.Get(), nullptr, IID_PPV_ARGS(list.GetAddressOf()));
        SUCCEEDED(hr)) {
        auto ins = radray::make_shared<CmdListD3D12>(std::move(list), _device, _type);
        return ins;
    } else {
        return std::nullopt;
    }
}

}  // namespace radray::render::d3d12
