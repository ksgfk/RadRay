#pragma once

#include <radray/core/object.h>

namespace radray::rhi {

class ICommandQueue;

class IFence : public Object {
public:
    ~IFence() noexcept override = default;

    virtual void Signal(ICommandQueue* queue, uint64_t fence) = 0;

    virtual void Wait(ICommandQueue* queue, uint64_t fence) = 0;

    virtual void IsComplete(uint64_t fence) = 0;

    virtual void Sync(uint64_t fence) = 0;
};

}  // namespace radray::rhi
