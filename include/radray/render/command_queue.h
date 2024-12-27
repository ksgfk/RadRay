#pragma once

#include <radray/render/common.h>

namespace radray::render {

class CommandQueue : public RenderBase {
public:
    virtual ~CommandQueue() noexcept = default;

    virtual void Submit(std::span<CommandBuffer*> buffers, Nullable<Fence> singalFence) noexcept = 0;

    virtual void Wait() noexcept = 0;

    virtual void WaitFences(std::span<Fence*> fences) noexcept = 0;
};

}  // namespace radray::render
