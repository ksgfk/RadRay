#include "d3d12_cmd_allocator.h"

#include "d3d12_device.h"
#include "d3d12_cmd_list.h"

namespace radray::render::d3d12 {

void CmdAllocatorD3D12::Destroy() noexcept {
    _cmdAlloc = nullptr;
}

std::optional<std::shared_ptr<CommandBuffer>> CmdAllocatorD3D12::CreateCommandBuffer() noexcept {
    ComPtr<ID3D12GraphicsCommandList> list;
    if (HRESULT hr = _device->_device->CreateCommandList(0, _type, _cmdAlloc.Get(), nullptr, IID_PPV_ARGS(list.GetAddressOf()));
        hr == S_OK) {
        auto ins = std::make_shared<CmdListD3D12>(_device, this, _type);
        ins->_cmdList = std::move(list);
        return ins;
    } else {
        return std::nullopt;
    }
}

}  // namespace radray::render::d3d12
