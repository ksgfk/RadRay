#pragma once

#include <radray/object.h>
#include <radray/rhi/common.h>

namespace radray::rhi {

class IDevice;

class IResource : public Object {
public:
    IResource(std::shared_ptr<IDevice> device) noexcept : _device(std::move(device)) {}
    ~IResource() noexcept override = default;

    IDevice* GetDevice() const noexcept { return _device.get(); }

private:
    std::shared_ptr<IDevice> _device;
};

}  // namespace radray::rhi
