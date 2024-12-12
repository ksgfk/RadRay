#pragma once

#include <radray/render/common.h>

namespace radray::render {

class CommandQueue : public RenderBase {
public:
    virtual ~CommandQueue() noexcept = default;

    virtual std::optional<radray::shared_ptr<CommandPool>> CreateCommandPool() noexcept = 0;
};

}  // namespace radray::render
