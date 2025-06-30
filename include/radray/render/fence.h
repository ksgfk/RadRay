#pragma once

#include <radray/render/common.h>

namespace radray::render {

class Fence : public RenderBase {
public:
    virtual ~Fence() noexcept = default;

    RenderObjectTags GetTag() const noexcept final { return RenderObjectTag::Fence; }

    virtual void Wait() noexcept = 0;
};

}  // namespace radray::render
