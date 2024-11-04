#pragma once

#include <radray/render/command_buffer.h>
#include "metal_helper.h"

namespace radray::render::metal {

class CmdBufferMetal : public CommandBuffer {
public:
    ~CmdBufferMetal() noexcept override = default;

    bool IsValid() const noexcept override { return _buffer.get() != nullptr; }
    void Destroy() noexcept override;

public:
    NS::SharedPtr<MTL::CommandBuffer> _buffer;
};

}  // namespace radray::render::metal
