#pragma once

#include <radray/object.h>

namespace radray::rhi {

class ICommandQueue;
class ITexture;
class IDevice;

class ISwapChain : public Object {
public:
    explicit ISwapChain(std::shared_ptr<IDevice> device) : _device(std::move(device)) {}
    ~ISwapChain() noexcept override = default;

    virtual void Present(ICommandQueue* queue) = 0;

    IDevice* GetDevice() const noexcept { return _device.get(); }

private:
    std::shared_ptr<IDevice> _device;
};

}  // namespace radray::rhi
