#pragma once

#include <radray/render/common.h>

namespace radray::render {

class Semaphore : public RenderBase {
public:
    virtual ~Semaphore() noexcept = default;

    RenderObjectTags GetTag() const noexcept final { return RenderObjectTag::Semaphore; }
};

}  // namespace radray::render
