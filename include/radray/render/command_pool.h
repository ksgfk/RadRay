#pragma once

#include <radray/render/common.h>

namespace radray::render {

class CommandPool : public RenderBase {
public:
    virtual ~CommandPool() noexcept = default;
};

}  // namespace radray::render
