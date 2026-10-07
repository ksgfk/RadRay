#pragma once

#include <span>
#include <radray/render/rhi.h>

namespace radray {

/// Borrowed only during the current application recording phase, on its recording thread.
class ICmdAllocator {
public:
    virtual ~ICmdAllocator() noexcept = default;
    [[nodiscard]] virtual render::CommandBuffer* Allocate() = 0;
    /// Ends CPU recording. Does not register, submit, or make commands reusable.
    virtual void Return(std::span<render::CommandBuffer*> commands) = 0;
};

}  // namespace radray
