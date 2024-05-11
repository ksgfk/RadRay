#pragma once

#include <radray/core/object.h>

namespace radray::rhi {

class ITexture : public Object {
public:
    ~ITexture() noexcept override = default;
};

}  // namespace radray::rhi
