#pragma once

#include <radray/object.h>

namespace radray::rhi {

class ICommandQueue;
class ITexture;

class ISwapChain : public Object {
public:
    ~ISwapChain() noexcept override = default;

    virtual void Present(ICommandQueue* queue) = 0;
};

}  // namespace radray::rhi
