#pragma once

#include <radray/render/command_queue.h>
#include "d3d12_helper.h"

namespace radray::render::d3d12 {

class CmdQueueD3D12 : public CommandQueue {
public:
    ~CmdQueueD3D12() noexcept override = default;

    bool IsValid() const noexcept override { return _queue.Get() != nullptr; }
    void Destroy() noexcept override;

    std::optional<std::shared_ptr<CommandPool>> CreateCommandPool(std::string_view debugName = "") noexcept override;

public:
    ComPtr<ID3D12CommandQueue> _queue;
};

}  // namespace radray::render::d3d12
