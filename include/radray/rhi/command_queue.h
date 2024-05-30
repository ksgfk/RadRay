#pragma once

#include <radray/object.h>
#include <radray/rhi/command_list.h>

namespace radray::rhi {

class IDevice;

class ICommandQueue : public Object {
public:
    ICommandQueue(std::shared_ptr<IDevice>&& device) noexcept : _device(std::move(device)) {}
    ~ICommandQueue() noexcept override = default;

    virtual void Sync() = 0;

    virtual void Execute(CommandList&& cmd) = 0;

    IDevice* GetDevice() const noexcept { return _device.get(); }

private:
    std::shared_ptr<IDevice> _device;
};

}  // namespace radray::rhi
