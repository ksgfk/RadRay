#pragma once

#include <radray/render/command_queue.h>
#include "d3d12_helper.h"

namespace radray::render::d3d12 {

class CmdQueueD3D12 : public CommandQueue {
public:
    CmdQueueD3D12(
        ComPtr<ID3D12CommandQueue> queue,
        DeviceD3D12* device,
        D3D12_COMMAND_LIST_TYPE type) noexcept
        : _queue(std::move(queue)),
          _device(device),
          _type(type) {}
    ~CmdQueueD3D12() noexcept override = default;

    bool IsValid() const noexcept override { return _queue.Get() != nullptr; }
    void Destroy() noexcept override;

public:
    ComPtr<ID3D12CommandQueue> _queue;
    DeviceD3D12* _device;
    D3D12_COMMAND_LIST_TYPE _type;
};

}  // namespace radray::render::d3d12
