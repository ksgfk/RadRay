#pragma once

#include <radray/render/command_buffer.h>
#include "metal_helper.h"

namespace radray::render::metal {

class DeviceMetal;

class CmdBufferMetal : public CommandBuffer {
public:
    explicit CmdBufferMetal(
        shared_ptr<DeviceMetal> device,
        NS::SharedPtr<MTL::CommandBuffer> buffer) noexcept
        : _device(std::move(device)),
          _buffer(std::move(buffer)) {}
    ~CmdBufferMetal() noexcept override = default;

    bool IsValid() const noexcept override { return _buffer.get() != nullptr; }
    void Destroy() noexcept override;

public:
    shared_ptr<DeviceMetal> _device;
    NS::SharedPtr<MTL::CommandBuffer> _buffer;
};

}  // namespace radray::render::metal
