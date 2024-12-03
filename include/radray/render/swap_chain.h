#pragma once

#include <radray/render/common.h>

namespace radray::render {

class SwapChain : public RenderBase {
public:
    virtual ~SwapChain() noexcept = default;
};

}  // namespace radray::render
