#pragma once

#include <radray/render/common.h>

namespace radray::render {

class SwapChain : public RenderBase {
public:
    virtual ~SwapChain() noexcept = default;

    virtual Texture* AcquireNextRenderTarget() noexcept = 0;
    virtual Texture* GetCurrentRenderTarget() noexcept = 0;
    virtual void Present() noexcept = 0;
};

}  // namespace radray::render
