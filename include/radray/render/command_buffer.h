#pragma once

#include <radray/render/common.h>

namespace radray::render {

class CommandBuffer : public RenderBase {
public:
    virtual ~CommandBuffer() noexcept = default;
};

}  // namespace radray::render
