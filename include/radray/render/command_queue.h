#pragma once

#include <radray/render/common.h>
#include <radray/render/command_pool.h>

namespace radray::render {

class CommandQueue : public RenderBase {
public:
    virtual ~CommandQueue() noexcept = default;

    virtual std::optional<std::shared_ptr<CommandPool>> CreateCommandPool() noexcept = 0;
};

}  // namespace radray::render
