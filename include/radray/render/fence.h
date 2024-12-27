#pragma once

#include <radray/render/common.h>

namespace radray::render {

class Fence : public RenderBase {
public:
    virtual ~Fence() noexcept = default;
};

}  // namespace radray::render
