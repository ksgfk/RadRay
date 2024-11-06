#pragma once

#include <radray/render/command_pool.h>
#include "metal_helper.h"

namespace radray::render::metal {

class CmdPoolMetal : public CommandPool {
public:
    explicit CmdPoolMetal(MTL::CommandQueue* q) noexcept : _queue(q) {}
    ~CmdPoolMetal() noexcept override = default;

    bool IsValid() const noexcept override { return _queue != nullptr; }
    void Destroy() noexcept override;

    std::optional<std::shared_ptr<CommandBuffer>> CreateCommandBuffer() noexcept override;

public:
    MTL::CommandQueue* _queue;
};

}  // namespace radray::render::metal
