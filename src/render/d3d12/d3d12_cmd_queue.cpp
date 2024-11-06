#include "d3d12_cmd_queue.h"

#include "d3d12_device.h"
#include "d3d12_cmd_allocator.h"

namespace radray::render::d3d12 {

void CmdQueueD3D12::Destroy() noexcept {
    _queue = nullptr;
}

std::optional<radray::shared_ptr<CommandPool>> CmdQueueD3D12::CreateCommandPool() noexcept {
    ComPtr<ID3D12CommandAllocator> alloc;
    if (HRESULT hr = _device->_device->CreateCommandAllocator(_type, IID_PPV_ARGS(alloc.GetAddressOf()));
        hr == S_OK) {
        auto ins = radray::make_shared<CmdAllocatorD3D12>(
            std::move(alloc),
            std::static_pointer_cast<DeviceD3D12>(_device->shared_from_this()),
            _type);
        return ins;
    } else {
        return std::nullopt;
    }
}

}  // namespace radray::render::d3d12
