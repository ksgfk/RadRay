#pragma once

#include <radray/core/object.h>

namespace radray::rhi {

class IBuffer : public Object {
public:
    ~IBuffer() noexcept override = default;
};

}  // namespace radray::rhi
