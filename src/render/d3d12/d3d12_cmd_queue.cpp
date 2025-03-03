#include "d3d12_cmd_queue.h"

#include "d3d12_device.h"
#include "d3d12_cmd_list.h"
#include "d3d12_fence.h"

namespace radray::render::d3d12 {

void CmdQueueD3D12::Destroy() noexcept {
    _queue = nullptr;
}

Nullable<radray::shared_ptr<CommandBuffer>> CmdQueueD3D12::CreateCommandBuffer() noexcept {
    ComPtr<ID3D12CommandAllocator> alloc;
    if (HRESULT hr = _device->_device->CreateCommandAllocator(_type, IID_PPV_ARGS(alloc.GetAddressOf()));
        FAILED(hr)) {
        RADRAY_ERR_LOG("cannot create ID3D12CommandAllocator, reason={} (code:{})", GetErrorName(hr), hr);
        return nullptr;
    }
    ComPtr<ID3D12GraphicsCommandList> list;
    if (HRESULT hr = _device->_device->CreateCommandList(0, _type, alloc.Get(), nullptr, IID_PPV_ARGS(list.GetAddressOf()));
        SUCCEEDED(hr)) {
        RADRAY_DX_CHECK(list->Close());
        return radray::make_shared<CmdListD3D12>(
            std::move(alloc),
            std::move(list),
            _type);
    } else {
        RADRAY_ERR_LOG("cannot create ID3D12GraphicsCommandList, reason={} (code:{})", GetErrorName(hr), hr);
        return nullptr;
    }
}

void CmdQueueD3D12::Submit(std::span<CommandBuffer*> buffers, Nullable<Fence> singalFence) noexcept {
    if (buffers.size() >= std::numeric_limits<UINT>::max()) {
        RADRAY_ERR_LOG("submit too many command buffers {}", buffers.size());
        return;
    }
    radray::vector<ID3D12CommandList*> lists{};
    lists.reserve(buffers.size());
    for (CommandBuffer* cmd : buffers) {
        auto list = static_cast<CmdListD3D12*>(cmd);
        lists.push_back(list->_cmdList.Get());
    }
    _queue->ExecuteCommandLists(static_cast<UINT>(lists.size()), lists.data());
    if (singalFence) {
        auto fence = static_cast<FenceD3D12*>(singalFence.Value());
        fence->_fenceValue++;
        RADRAY_DX_CHECK(_queue->Signal(fence->_fence.Get(), fence->_fenceValue));
    }
}

void CmdQueueD3D12::Wait() noexcept {
    _fence->_fenceValue++;
    RADRAY_DX_CHECK(_queue->Signal(_fence->_fence.Get(), _fence->_fenceValue));
    _fence->Wait();
}

void CmdQueueD3D12::WaitFences(std::span<Fence*> fences) noexcept {
    radray::vector<HANDLE> events;
    for (Fence* i : fences) {
        FenceD3D12* f = static_cast<FenceD3D12*>(i);
        f->_fenceValue++;
        RADRAY_DX_CHECK(_queue->Signal(_fence->_fence.Get(), _fence->_fenceValue));
        RADRAY_DX_CHECK(f->_fence->SetEventOnCompletion(f->_fenceValue, f->_event.Get()));
        events.emplace_back(f->_event.Get());
    }
    if (fences.size() > MAXIMUM_WAIT_OBJECTS) {
        for (HANDLE e : events) {
            WaitForSingleObject(e, INFINITE);
        }
    } else {
        WaitForMultipleObjects(static_cast<DWORD>(events.size()), events.data(), TRUE, INFINITE);
    }
}

}  // namespace radray::render::d3d12
