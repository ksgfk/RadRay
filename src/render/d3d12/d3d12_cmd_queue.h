#pragma once

#include <radray/render/command_queue.h>
#include "d3d12_helper.h"

namespace radray::render::d3d12 {

class CmdQueueD3D12 : public CommandQueue {
public:
    CmdQueueD3D12(
        ComPtr<ID3D12CommandQueue> queue,
        D3D12_COMMAND_LIST_TYPE type,
        std::shared_ptr<FenceD3D12> fence) noexcept
        : _queue(std::move(queue)),
          _fence(std::move(fence)),
          _type(type) {}
    ~CmdQueueD3D12() noexcept override = default;

    bool IsValid() const noexcept override { return _queue.Get() != nullptr; }
    void Destroy() noexcept override;

    void Submit(std::span<CommandBuffer*> buffers, Nullable<Fence> singalFence) noexcept override;

    void Wait() noexcept override;

    void WaitFences(std::span<Fence*> fences) noexcept override;

public:
    ComPtr<ID3D12CommandQueue> _queue;
    std::shared_ptr<FenceD3D12> _fence;
    D3D12_COMMAND_LIST_TYPE _type;
};

}  // namespace radray::render::d3d12
