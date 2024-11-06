#pragma once

#include <radray/render/common.h>
#include <radray/render/command_buffer.h>

namespace radray::render {

class CommandBuffer;

class CommandPool : public RenderBase {
public:
    virtual ~CommandPool() noexcept = default;

    virtual std::optional<radray::shared_ptr<CommandBuffer>> CreateCommandBuffer() noexcept = 0;
};

}  // namespace radray::render
