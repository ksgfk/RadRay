#pragma once

#include <radray/render/common.h>

namespace radray::render {

class DescriptorSet : public RenderBase {
public:
    virtual ~DescriptorSet() noexcept = default;

    // virtual void SetResources(std::span<ResourceView*> views) noexcept = 0;
};

}  // namespace radray::render
