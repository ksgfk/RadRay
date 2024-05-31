#pragma once

#include <radray/object.h>
#include <radray/rhi/resource.h>

namespace radray::rhi {

class ITexture : public IResource {
public:
    ITexture(std::shared_ptr<IDevice> device) noexcept : IResource(std::move(device)) {}
    ~ITexture() noexcept override = default;
};

}  // namespace radray::rhi
