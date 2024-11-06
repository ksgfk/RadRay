#pragma once

#include <radray/render/command_pool.h>
#include "metal_helper.h"

namespace radray::render::metal {

class DeviceMetal;

class CmdPoolMetal : public CommandPool {
public:
    explicit CmdPoolMetal(
        radray::shared_ptr<DeviceMetal> device,
        MTL::CommandQueue* q) noexcept
        : _device(std::move(device)),
          _queue(q) {}
    ~CmdPoolMetal() noexcept override = default;

    bool IsValid() const noexcept override { return _queue != nullptr; }
    void Destroy() noexcept override;

    std::optional<radray::shared_ptr<CommandBuffer>> CreateCommandBuffer() noexcept override;

public:
    radray::shared_ptr<DeviceMetal> _device;
    MTL::CommandQueue* _queue;
};

}  // namespace radray::render::metal
