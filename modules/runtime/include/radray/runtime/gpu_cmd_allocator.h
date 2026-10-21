#pragma once

#include <span>
#include <radray/render/rhi.h>

namespace radray {

/// Borrowed only during the current application recording phase, on its recording thread.
class IGpuCmdAllocator {
public:
    virtual ~IGpuCmdAllocator() noexcept = default;

    [[nodiscard]] virtual render::CommandBuffer* Allocate() = 0;

    /// Ends CPU recording. Does not register, submit, or make commands reusable.
    virtual void Return(std::span<render::CommandBuffer*> commands) = 0;
    /// Abandons unaccepted commands. Native references remain pinned until the epoch ends.
    virtual void Discard(std::span<render::CommandBuffer*> commands) = 0;
};

/// Accepts closed resource-update commands into the current flight. Accepted work must execute.
class IRequiredGpuWork {
public:
    virtual ~IRequiredGpuWork() noexcept = default;
    virtual void RegisterRequired(std::span<render::CommandBuffer*> commands) = 0;
};

}  // namespace radray
