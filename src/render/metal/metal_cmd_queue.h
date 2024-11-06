#pragma once

#include <radray/render/command_queue.h>
#include "metal_helper.h"

namespace radray::render::metal {

class CmdQueueMetal : public CommandQueue {
public:
    explicit CmdQueueMetal(NS::SharedPtr<MTL::CommandQueue> queue) : _queue(std::move(queue)) {}
    ~CmdQueueMetal() noexcept override = default;

    bool IsValid() const noexcept override { return _queue.get() != nullptr; }
    void Destroy() noexcept override;

    std::optional<std::shared_ptr<CommandPool>> CreateCommandPool() noexcept override;

public:
    NS::SharedPtr<MTL::CommandQueue> _queue;
};

}  // namespace radray::render::metal
