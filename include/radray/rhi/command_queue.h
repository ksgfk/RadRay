#pragma once

#include <radray/object.h>
#include <radray/rhi/command_list.h>

namespace radray::rhi {

class ICommandQueue : public Object {
public:
    ~ICommandQueue() noexcept override = default;

    virtual void Sync() = 0;

    virtual void Execute(CommandList&& cmd) = 0;
};

}  // namespace radray::rhi
