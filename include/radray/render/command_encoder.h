#pragma once

#include <radray/render/common.h>

namespace radray::render {

class CommandEncoder : public RenderBase {
public:
    virtual ~CommandEncoder() noexcept = default;
};

}  // namespace radray::render
