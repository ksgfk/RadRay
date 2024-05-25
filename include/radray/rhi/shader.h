#pragma once

#include <radray/object.h>

namespace radray::rhi {

class IShader : public Object {
public:
    ~IShader() noexcept override = default;
};

}  // namespace radray::rhi
