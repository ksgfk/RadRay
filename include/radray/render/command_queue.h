#pragma once

#include <radray/render/common.h>

namespace radray::render {

class CommandQueue : public RenderBase {
public:
    virtual ~CommandQueue() noexcept = default;
};

}  // namespace radray::render
